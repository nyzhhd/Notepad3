// Scintilla source code edit control
/**
 * @file LexJSON.cxx
 * @date February 19, 2016
 * @brief Lexer for JSON and JSON-LD formats
 * @author nkmathew
 *
 * The License.txt file describes the conditions under which this software may
 * be distributed.
 *
 */

#include <cstdlib>
#include <cassert>
#include <cctype>
#include <cstdio>

#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <functional>

#include "ILexer.h"
#include "Scintilla.h"
#include "WordList.h"
#include "LexAccessor.h"
#include "StyleContext.h"
#include "LexerModule.h"
#include "OptionSet.h"
#include "DefaultLexer.h"

#include "CharSetX.h"
#include "SciXLexer.h"

using namespace Scintilla;
using namespace Lexilla;

static const char *const JSONWordListDesc[] = {
	"JSON Keywords",
	"JSON-LD Keywords",
	nullptr
};

/**
 * Used to detect compact IRI/URLs in JSON-LD without first looking ahead for the
 * colon separating the prefix and suffix
 *
 * https://www.w3.org/TR/json-ld/#dfn-compact-iri
 */
struct CompactIRI {
	int colonCount;
	bool foundInvalidChar;
	CharacterSet setCompactIRI;
	CompactIRI() {
		colonCount = 0;
		foundInvalidChar = false;
		setCompactIRI = CharacterSet(CharacterSet::setAlpha, "$_-");
	}
	void resetState() {
		colonCount = 0;
		foundInvalidChar = false;
	}
	void checkChar(int ch) {
		if (ch == ':') {
			colonCount++;
		} else {
			foundInvalidChar |= !setCompactIRI.Contains(ch);
		}
	}
	bool shouldHighlight() const {
		return !foundInvalidChar && colonCount == 1;
	}
};

/**
 * Keeps track of escaped characters in strings as per:
 *
 * https://tools.ietf.org/html/rfc7159#section-7
 */
struct EscapeSequence {
	int digitsLeft;
	CharacterSet setHexDigits;
	CharacterSet setEscapeChars;
	EscapeSequence() {
		digitsLeft = 0;
		setHexDigits = CharacterSet(CharacterSet::setDigits, "ABCDEFabcdef");
		setEscapeChars = CharacterSet(CharacterSet::setNone, "\\\"tnbfru/");
	}
	// Returns true if the following character is a valid escaped character
	bool newSequence(int nextChar) {
		digitsLeft = 0;
		if (nextChar == 'u') {
			digitsLeft = 5;
		} else if (!setEscapeChars.Contains(nextChar)) {
			return false;
		}
		return true;
	}
	bool atEscapeEnd() const {
		return digitsLeft <= 0;
	}
	bool isInvalidChar(int currChar) const {
		return !setHexDigits.Contains(currChar);
	}
};

struct OptionsJSON {
	bool foldCompact;
	bool fold;
	bool allowComments;
	bool escapeSequence;
	OptionsJSON() {
		foldCompact = true;
		fold = true;
		allowComments = true;
		escapeSequence = true;
	}
};

struct OptionSetJSON : public OptionSet<OptionsJSON> {
	OptionSetJSON() {
		DefineProperty("lexer.json.escape.sequence", &OptionsJSON::escapeSequence,
					   "Set to 1 to enable highlighting of escape sequences in strings");

		DefineProperty("lexer.json.allow.comments", &OptionsJSON::allowComments,
					   "Set to 1 to enable highlighting of line/block comments in JSON");

		DefineProperty("fold.compact", &OptionsJSON::foldCompact);
		DefineProperty("fold", &OptionsJSON::fold);
		DefineWordListSets(JSONWordListDesc);
	}
};

class LexerJSON : public DefaultLexer {
	OptionsJSON options;
	OptionSetJSON optSetJSON;
	EscapeSequence escapeSeq;
	WordList keywordsJSON;
	WordList keywordsJSON_LD;
	CharacterSet setOperators;
	CharacterSet setURL;
	CharacterSet setKeywordJSON_LD;
	CharacterSet setKeywordJSON;
	CompactIRI compactIRI;

	static bool IsNextNonWhitespace(LexAccessor &styler, Sci_PositionU start, char ch) {
		Sci_PositionU i = 0;
		while (++i < 60) {
			char const curr = styler.SafeGetCharAt(start+i, '\0');
			char const next = styler.SafeGetCharAt(start+i+1, '\0');
			bool const atEOL = (curr == '\r' && next != '\n') || (curr == '\n');
			if (curr == ch) {
				return true;
			} else if (!isspacechar(curr) || atEOL) {
				return false;
			}
		}
		return false;
	}

	static bool IsOnlyWhitespaceTillLnEnd(LexAccessor& styler, Sci_PositionU start) {
		Sci_PositionU i = 0;
		while (true) {
			char const curr = styler.SafeGetCharAt(start + i, '\0');
			char const next = styler.SafeGetCharAt(start + i + 1, '\0');
			bool const atEOL = (curr == '\r' && next != '\n') || (curr == '\n');
			if (!curr || atEOL) { break; }
			if (!isspacechar(curr)) { return false; }
			++i;
		}
		return true;
	}

	/**
	 * Looks for the colon following the end quote
	 *
	 * Assumes property names of lengths no longer than a 120 characters.
	 * The colon is also expected to be less than 50 spaces after the end
	 * quote for the string to be considered a property name
	 */
	static constexpr bool IsPropChar(int ch) noexcept {
		return IsAlphaNumeric(ch); // || ch == '_';
	}

	static bool AtPropertyName(LexAccessor &styler, const Sci_PositionU start, bool bQuoted) {
		Sci_PositionU i = 0;
		bool escaped = false;
		while (++i < 120) {
			char curr = styler.SafeGetCharAt(start+i, '\0');
			if (escaped) {
				escaped = false;
				continue;
			}
			escaped = (curr == '\\');
			if (curr == ':' && !bQuoted) {
				return true;
			} else if ((curr == '"' || curr == '\'') && bQuoted) {
				return IsNextNonWhitespace(styler, start + i, ':');
			} else if (isspacechar(curr) && !bQuoted) {
				return IsNextNonWhitespace(styler, start + i, ':');
			} if (!curr || (!bQuoted && !IsPropChar(curr))) {
				return false;
			}
		}
		return false;
	}

	static bool IsNextWordInList(WordList &keywordList, CharacterSet wordSet,
								 const StyleContext& context, LexAccessor& styler) {
		char word[51];
		Sci_Position currPos = (Sci_Position) context.currentPos;
		int i = 0;
		while (i < 50) {
			char const ch = styler.SafeGetCharAt(currPos + i);
			if (!wordSet.Contains(ch)) {
				break;
			}
			word[i] = ch;
			++i;
		}
		word[i] = '\0';
		return keywordList.InList(word);
	}

	static bool IsJSONNumber(const StyleContext& context, bool bChPrevIsOp) {

		bool numberStart =
			IsADigit(context.ch) && (context.chPrev == '+' ||
				context.chPrev == '-' ||
				context.atLineStart ||
				IsASpace(context.chPrev) ||
				bChPrevIsOp);
		bool hexaStart =
			tolower(context.ch) == 'x' &&
			context.chPrev == '0' &&
			IsADigit(context.chNext, 16);
		bool hexaPart =
			IsADigit(context.ch, 16) &&
			(IsADigit(context.chPrev, 16) || tolower(context.chPrev) == 'x');
		bool exponentPart =
			tolower(context.ch) == 'e' &&
			IsADigit(context.chPrev) &&
			(IsADigit(context.chNext) ||
				context.chNext == '+' ||
				context.chNext == '-');
		bool signPart =
			(context.ch == '-' || context.ch == '+') &&
			((tolower(context.chPrev) == 'e' && IsADigit(context.chNext)) ||
				((IsASpace(context.chPrev) || bChPrevIsOp)
					&& IsADigit(context.chNext)));
		bool adjacentDigit =
			IsADigit(context.ch) && IsADigit(context.chPrev);
		bool afterExponent = IsADigit(context.ch) && tolower(context.chPrev) == 'e';
		bool dotPart = context.ch == '.' &&
			(IsADigit(context.chPrev) || IsASpace(context.chPrev)) &&
			(IsADigit(context.chNext) || IsASpace(context.chNext) || context.chNext == ',');
		bool afterDot = IsADigit(context.ch) && context.chPrev == '.';

		return (
			numberStart ||
			hexaStart || hexaPart ||
			exponentPart ||
			signPart ||
			adjacentDigit ||
			dotPart ||
			afterExponent ||
			afterDot
		);
	}


	public:
	LexerJSON() :
		DefaultLexer("json", SCLEX_JSON),
		setOperators(CharacterSet::setNone, "[{}]:,"),
		setURL(CharacterSet::setAlphaNum, "-._~:/?#[]@!$&'()*+,),="),
		setKeywordJSON_LD(CharacterSet::setAlpha, ":@"),
		setKeywordJSON(CharacterSet::setAlpha, "$_+-") {
	}
    virtual ~LexerJSON() = default;
	int SCI_METHOD Version() const override {
		return lvRelease5;
	}
	void SCI_METHOD Release() override {
		delete this;
	}
	const char *SCI_METHOD PropertyNames() override {
		return optSetJSON.PropertyNames();
	}
	int SCI_METHOD PropertyType(const char *name) override {
		return optSetJSON.PropertyType(name);
	}
	const char *SCI_METHOD DescribeProperty(const char *name) override {
		return optSetJSON.DescribeProperty(name);
	}
	Sci_Position SCI_METHOD PropertySet(const char *key, const char *val) override {
		if (optSetJSON.PropertySet(&options, key, val)) {
			return 0;
		}
		return -1;
	}
	const char * SCI_METHOD PropertyGet(const char *key) override {
		return optSetJSON.PropertyGet(key);
	}
	Sci_Position SCI_METHOD WordListSet(int n, const char *wl) override {
		WordList* wordListN = nullptr;
		switch (n) {
			case 0:
				wordListN = &keywordsJSON;
				break;
			case 1:
				wordListN = &keywordsJSON_LD;
				break;
		}
		Sci_Position firstModification = -1;
		if (wordListN) {
			WordList wlNew;
			wlNew.Set(wl);
			if (*wordListN != wlNew) {
				wordListN->Set(wl);
				firstModification = 0;
			}
		}
		return firstModification;
	}
	void *SCI_METHOD PrivateCall(int, void *) override {
		return 0;
	}
	static ILexer5 *LexerFactoryJSON() {
		return new LexerJSON;
	}
	const char *SCI_METHOD DescribeWordListSets() override {
		return optSetJSON.DescribeWordListSets();
	}
	void SCI_METHOD Lex(Sci_PositionU startPos,
								Sci_Position length,
								int initStyle,
								IDocument *pAccess) override;
	void SCI_METHOD Fold(Sci_PositionU startPos,
								 Sci_Position length,
								 int initStyle,
								 IDocument *pAccess) override;
};

void SCI_METHOD LexerJSON::Lex(Sci_PositionU startPos,
							   Sci_Position length,
							   int initStyle,
							   IDocument *pAccess) {
	LexAccessor styler(pAccess);
	StyleContext context(startPos, length, initStyle, styler);
	int stringStyleBefore = SCE_JSON_STRING;
	bool doubleQuotCntx = false;
	bool singleQuotCntx = false;
	while (context.More()) {
		switch (context.state) {
			case SCE_JSON_BLOCKCOMMENT:
				if (context.Match("*/")) {
					context.Forward();
					context.ForwardSetState(SCE_JSON_DEFAULT);
				}
				break;
			case SCE_JSON_LINECOMMENT:
				if (context.atLineEnd) {
					context.SetState(SCE_JSON_DEFAULT);
				}
				break;
			case SCE_JSON_STRINGEOL:
				if (context.atLineStart) {
					context.SetState(SCE_JSON_DEFAULT);
				}
				break;
			case SCE_JSON_ESCAPESEQUENCE:
				escapeSeq.digitsLeft--;
				if (!escapeSeq.atEscapeEnd()) {
					if (escapeSeq.isInvalidChar(context.ch)) {
						context.SetState(SCE_JSON_ERROR);
					}
					break;
				}
				if (context.ch == '"' || context.ch == '\'') {
					context.SetState(stringStyleBefore);
					context.ForwardSetState(SCE_JSON_DEFAULT);
				} else if (context.ch == '\\') {
					if (!escapeSeq.newSequence(context.chNext)) {
						context.SetState(SCE_JSON_ERROR);
					}
					context.Forward();
				} else {
					context.SetState(stringStyleBefore);
					if (context.atLineEnd) {
						context.ChangeState(SCE_JSON_STRINGEOL);
					}
				}
				break;
            case SCE_JSON_PROPERTYNAME:
                [[fallthrough]];
            case SCE_JSON_STRING:
                if (context.ch == ':' && !(doubleQuotCntx || singleQuotCntx)) {
					context.SetState(SCE_JSON_OPERATOR);
				} else if (context.ch == '"' && doubleQuotCntx) {
					if (compactIRI.shouldHighlight()) {
						context.ChangeState(SCE_JSON_COMPACTIRI);
						context.ForwardSetState(SCE_JSON_DEFAULT);
						compactIRI.resetState();
					} else {
						context.ForwardSetState(SCE_JSON_DEFAULT);
					}
					doubleQuotCntx = false;
				}
				else if (context.ch == '\'' && singleQuotCntx) {
					if (compactIRI.shouldHighlight()) {
						context.ChangeState(SCE_JSON_COMPACTIRI);
						context.ForwardSetState(SCE_JSON_DEFAULT);
						compactIRI.resetState();
					}
					else {
						context.ForwardSetState(SCE_JSON_DEFAULT);
					}
					singleQuotCntx = false;
				} else if (context.ch == '\\') {
                    // line continuation (yet: LF and CRLF only) ?
					if (context.Match("\\\n")) {
						context.Forward();
						context.ForwardSetState(context.state);
						continue;
					}
					else if (context.Match("\\\r\n")) {
						context.Forward();
						context.Forward();
						context.ForwardSetState(context.state);
						continue;
					}
					else {
						stringStyleBefore = context.state;
						if (options.escapeSequence) {
							context.SetState(SCE_JSON_ESCAPESEQUENCE);
							if (!escapeSeq.newSequence(context.chNext)) {
								context.SetState(SCE_JSON_ERROR);
							}
						}
						context.Forward();
					}
				} else if (context.atLineEnd) {
					context.ChangeState(SCE_JSON_STRINGEOL);
				} else if (context.Match("https://") ||
						   context.Match("http://") ||
						   context.Match("ssh://") ||
						   context.Match("git://") ||
						   context.Match("svn://") ||
						   context.Match("ftp://") ||
						   context.Match("mailto:")) {
					// Handle most common URI schemes only
					stringStyleBefore = context.state;
					context.SetState(SCE_JSON_URI);
				} else if (context.ch == '@') {
					// https://www.w3.org/TR/json-ld/#dfn-keyword
					if (IsNextWordInList(keywordsJSON_LD, setKeywordJSON_LD, context, styler)) {
						stringStyleBefore = context.state;
						context.SetState(SCE_JSON_LDKEYWORD);
					}
				}
				else if (IsPropChar(context.ch)) {
					if (!AtPropertyName(styler, context.currentPos, (doubleQuotCntx || singleQuotCntx))) {
						if (context.state == SCE_JSON_PROPERTYNAME) {
							context.SetState(SCE_JSON_ERROR);
						}
					}
				}
				else {
					compactIRI.checkChar(context.ch);
				}
				break;
			case SCE_JSON_LDKEYWORD:
			case SCE_JSON_URI:
				if ((!setKeywordJSON_LD.Contains(context.ch) &&
					 (context.state == SCE_JSON_LDKEYWORD)) ||
					(!setURL.Contains(context.ch))) {
					context.SetState(stringStyleBefore);
				}
				if (context.ch == '"' || context.ch == '\'') {
					context.ForwardSetState(SCE_JSON_DEFAULT);
				} else if (context.atLineEnd) {
					context.ChangeState(SCE_JSON_STRINGEOL);
				}
				break;
			case SCE_JSON_OPERATOR:
			case SCE_JSON_NUMBER:
				context.SetState(SCE_JSON_DEFAULT);
				break;
			case SCE_JSON_ERROR:
				if (context.atLineEnd) {
					context.SetState(SCE_JSON_DEFAULT);
				}
				break;
			case SCE_JSON_KEYWORD:
				if (!setKeywordJSON.Contains(context.ch)) {
					context.SetState(SCE_JSON_DEFAULT);
				}
				break;
		}

		if (context.state == SCE_JSON_DEFAULT) {
			if (context.ch == '"') {
				compactIRI.resetState();
				context.SetState(SCE_JSON_STRING);
				doubleQuotCntx = !singleQuotCntx;
				if (AtPropertyName(styler, context.currentPos, true)) {
					context.SetState(SCE_JSON_PROPERTYNAME);
				}
			} else if (context.ch == '\'') {
				compactIRI.resetState();
				context.SetState(SCE_JSON_STRING);
				singleQuotCntx = !doubleQuotCntx;
				if (AtPropertyName(styler, context.currentPos, true)) {
					context.SetState(SCE_JSON_PROPERTYNAME);
				}
			} else if (setKeywordJSON.Contains(context.ch)) {
				if (IsNextWordInList(keywordsJSON, setKeywordJSON, context, styler)) {
					context.SetState(SCE_JSON_KEYWORD);
				}
			} else if (setOperators.Contains(context.ch)) {
				context.SetState(SCE_JSON_OPERATOR);
			} else if (options.allowComments && context.Match("/*")) {
				context.SetState(SCE_JSON_BLOCKCOMMENT);
				context.Forward();
			} else if (options.allowComments && context.Match("//")) {
				context.SetState(SCE_JSON_LINECOMMENT);
			}

			if (IsJSONNumber(context, setOperators.Contains(context.chPrev))) {
				context.SetState(SCE_JSON_NUMBER);
			}
			else if (context.state == SCE_JSON_DEFAULT) {
				if (IsPropChar(context.ch)) {
					if (AtPropertyName(styler, context.currentPos, (doubleQuotCntx || singleQuotCntx))) {
						context.SetState(SCE_JSON_PROPERTYNAME);
					}
				}
				else if (context.state == SCE_JSON_DEFAULT && !IsASpace(context.ch)) {
					context.SetState(SCE_JSON_ERROR);
				}
			}
		}
		context.Forward();
	}
	context.Complete();
}

void SCI_METHOD LexerJSON::Fold(Sci_PositionU startPos,
								Sci_Position length,
								int,
								IDocument *pAccess) {
	if (!options.fold) {
		return;
	}
	LexAccessor styler(pAccess);
	Sci_PositionU currLine = styler.GetLine(startPos);
	Sci_PositionU endPos = startPos + length;
	int currLevel = SC_FOLDLEVELBASE;
	if (currLine > 0)
		currLevel = styler.LevelAt(currLine - 1) >> 16;
	int nextLevel = currLevel;
	int visibleChars = 0;
	for (Sci_PositionU i = startPos; i < endPos; i++) {
		char curr = styler.SafeGetCharAt(i);
		char next = styler.SafeGetCharAt(i+1);
		bool atEOL = (curr == '\r' && next != '\n') || (curr == '\n');
		if (styler.StyleAt(i) == SCE_JSON_OPERATOR) {
			if (curr == '{' || curr == '[') {
				nextLevel++;
			} else if (curr == '}' || curr == ']') {
				nextLevel--;
			}
		}
		if (atEOL || i == (endPos-1)) {
			int level = currLevel | nextLevel << 16;
			if (!visibleChars && options.foldCompact) {
				level |= SC_FOLDLEVELWHITEFLAG;
			} else if (nextLevel > currLevel) {
				level |= SC_FOLDLEVELHEADERFLAG;
			}
			if (level != styler.LevelAt(currLine)) {
				styler.SetLevel(currLine, level);
			}
			currLine++;
			currLevel = nextLevel;
			visibleChars = 0;
		}
		if (!isspacechar(curr)) {
			visibleChars++;
		}
	}
}

LexerModule lmJSON(SCLEX_JSON,
				   LexerJSON::LexerFactoryJSON,
				   "json",
				   JSONWordListDesc);
