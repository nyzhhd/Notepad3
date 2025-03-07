/******************************************************************
 *  LexMarkdown.cxx
 *
 *  A simple Markdown lexer for scintilla.
 *
 *  Includes highlighting for some extra features from the
 *  Pandoc implementation; strikeout, using '#.' as a default
 *  ordered list item marker, and delimited code blocks.
 *
 *  Limitations:
 *
 *  Standard indented code blocks are not highlighted at all,
 *  as it would conflict with other indentation schemes. Use
 *  delimited code blocks for blanket highlighting of an
 *  entire code block.  Embedded HTML is not highlighted either.
 *  Blanket HTML highlighting has issues, because some Markdown
 *  implementations allow Markdown markup inside of the HTML. Also,
 *  there is a following blank line issue that can't be ignored,
 *  explained in the next paragraph. Embedded HTML and code
 *  blocks would be better supported with language specific
 *  highlighting.
 *
 *  The highlighting aims to accurately reflect correct syntax,
 *  but a few restrictions are relaxed. Delimited code blocks are
 *  highlighted, even if the line following the code block is not blank.
 *  Requiring a blank line after a block, breaks the highlighting
 *  in certain cases, because of the way Scintilla ends up calling
 *  the lexer.
 *
 *  Written by Jon Strait - jstrait@moonloop.net
 *
 *  The License.txt file describes the conditions under which this
 *  software may be distributed.
 *
 *****************************************************************/

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <assert.h>

#include <string>
#include <string_view>

#include "ILexer.h"
#include "Scintilla.h"
#include "SciLexer.h"

#include "WordList.h"
#include "LexAccessor.h"
#include "Accessor.h"
#include "StyleContext.h"
//#include "CharacterSet.h"
#include "CharSetX.h"
#include "LexerModule.h"

using namespace Lexilla;

// True if can follow ch down to the end with possibly trailing whitespace
static bool FollowToLineEnd(const int ch, const int state, const Sci_PositionU endPos, StyleContext &sc) {
    Sci_Position i = 0;
    while (sc.GetRelative(++i) == ch)
        ;
    // Skip over whitespace
    while (IsASpaceOrTab(sc.GetRelative(i)) && sc.currentPos + i < endPos)
        ++i;
    if (IsLineBreak(sc.GetRelative(i)) || sc.currentPos + i == endPos) {
        sc.Forward(i);
        sc.ChangeState(state);
        sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
        return true;
    }
    else return false;
}

#if 0

// Set the state on text section from current to length characters,
// then set the rest until the newline to default, except for any characters matching token
static void SetStateAndZoom(const int state, const Sci_Position length, const int token, StyleContext &sc) {
    sc.SetState(state);
    sc.Forward(length);
    sc.SetState(SCE_MARKDOWN_DEFAULT);
    sc.Forward();
    bool started = false;
    while (sc.More() && !IsNewline(sc.ch)) {
        if (sc.ch == token && !started) {
            sc.SetState(state);
            started = true;
        }
        else if (sc.ch != token) {
            sc.SetState(SCE_MARKDOWN_DEFAULT);
            started = false;
        }
        sc.Forward();
    }
    sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
}

#else

static void SetStateAndZoom(const int state, const Sci_Position length, const int token, StyleContext& sc) {
    sc.SetState(state);
    sc.Forward(length);
    bool started = false;
    while (sc.More() && !sc.atLineStart) {
        if (sc.ch == token) {
            sc.SetState(state);
            started = !started;
        }
        else {
            sc.SetState(started ? SCE_MARKDOWN_HDRTEXT : state);
        }
        sc.Forward();
    }
    sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
}

#endif

// Does the previous line have more than spaces and tabs?
static bool HasPrevLineContent(StyleContext &sc) {
    Sci_Position i = 0;
    // Go back to the previous newline
    while ((--i + (Sci_Position)sc.currentPos) >= 0 && !IsLineBreak(sc.GetRelative(i)))
        ;
    while ((--i + (Sci_Position)sc.currentPos) >= 0) {
        const int ch = sc.GetRelative(i);
        if (ch == '\n')
            break;
        if (!((ch == '\r' || IsASpaceOrTab(ch))))
            return true;
    }
    return false;
}

constexpr bool AtTermStart(StyleContext &sc) {
    return sc.currentPos == 0 || sc.chPrev == 0 || isspacechar(sc.chPrev);
}

static bool IsCompleteStyleRegion(StyleContext &sc, const char *token) {
    bool found = false;
	const size_t start = strlen(token);
    Sci_Position i = static_cast<Sci_Position>(start);
    while (!IsLineBreak(sc.GetRelative(i))) {
        // make sure an empty pair of single-char tokens doesn't match
        // with a longer token: {*}{*} != {**}
	    if (sc.GetRelative(i) == *token && sc.GetRelative(i - 1) != *token) {
		    found = start > 1U ? sc.GetRelative(i + 1) == token[1] : true;
			break;
		}
        i++;
    }
    return AtTermStart(sc) && found;
}

static bool IsValidHrule(const Sci_PositionU endPos, StyleContext &sc) {
    int count = 1;
    Sci_Position i = 0;
    for (;;) {
        ++i;
        int const ch = sc.GetRelative(i);
        if (ch == sc.ch)
            ++count;
        // hit a terminating character
        else if (!IsASpaceOrTab(ch) || (sc.currentPos + i) == endPos) {
            // Are we a valid HRULE
            if ((IsLineBreak(ch) || (sc.currentPos + i) == endPos) &&
                count >= 3 && !HasPrevLineContent(sc)) {
                sc.SetState(SCE_MARKDOWN_HRULE);
                sc.Forward(i);
                //@@@sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
                sc.SetState(SCE_MARKDOWN_DEFAULT);
                return true;
            }
            else {
                sc.SetState(SCE_MARKDOWN_DEFAULT);
                return false;
            }
        }
    }
}

constexpr bool IsInHeaderState(const int state) {
    return (
        state == SCE_MARKDOWN_HEADER1 || state == SCE_MARKDOWN_HEADER2 ||
        state == SCE_MARKDOWN_HEADER3 || state == SCE_MARKDOWN_HEADER4 ||
        state == SCE_MARKDOWN_HEADER5 || state == SCE_MARKDOWN_HEADER6
        );
}

static void ColorizeMarkdownDoc(Sci_PositionU startPos, Sci_Position length, int initStyle,
                               WordList **, Accessor &styler) {
    Sci_PositionU endPos = startPos + length;
    int precharCount = 0;
    bool isLinkNameDetecting = false;
    // Don't advance on a new loop iteration and retry at the same position.
    // Useful in the corner case of having to start at the beginning file position
    // in the default state.
    bool freezeCursor = false;

    StyleContext sc(startPos, static_cast<Sci_PositionU>(length), initStyle, styler);

    while (sc.More()) {
        // Skip past escaped characters
        if (sc.ch == '\\') {
            sc.Forward();
            continue;
        }

        if (IsInHeaderState(sc.state) && sc.atLineStart) {
            sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
        }

        // A blockquotes resets the line semantics
        if (sc.state == SCE_MARKDOWN_BLOCKQUOTE)
            sc.SetState(SCE_MARKDOWN_LINE_BEGIN);

        // Conditional state-based actions
        if (sc.state == SCE_MARKDOWN_CODE2) {
            if (sc.Match("``")) {
                const int closingSpan = (sc.GetRelative(2) == '`') ? 3 : 2;
                sc.Forward(closingSpan);
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
        }
        else if (sc.state == SCE_MARKDOWN_CODE) {
            if (sc.ch == '`' && sc.chPrev != ' ')
                sc.ForwardSetState(SCE_MARKDOWN_DEFAULT);
        }
        /* De-activated because it gets in the way of other valid indentation
         * schemes, for example multiple paragraphs inside a list item.
        // Code block
        else if (sc.state == SCE_MARKDOWN_CODEBK) {
            bool d = true;
            if (IsNewline(sc.ch)) {
                if (sc.chNext != '\t') {
                    for (int c = 1; c < 5; ++c) {
                        if (sc.GetRelative(c) != ' ')
                            d = false;
                    }
                }
            }
            else if (sc.atLineStart) {
                if (sc.ch != '\t' ) {
                    for (int i = 0; i < 4; ++i) {
                        if (sc.GetRelative(i) != ' ')
                            d = false;
                    }
                }
            }
            if (!d)
                sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
        }
        */
        // Strong
        else if (sc.state == SCE_MARKDOWN_STRONG1) {
            if ((sc.Match("**") && sc.chPrev != ' ') || IsLineBreak(sc.GetRelative(2))) {
                sc.Forward(2);
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
        }
        else if (sc.state == SCE_MARKDOWN_STRONG2) {
            if ((sc.Match("__") && sc.chPrev != ' ') || IsLineBreak(sc.GetRelative(2))) {
                sc.Forward(2);
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
        }
        // Emphasis
        else if (sc.state == SCE_MARKDOWN_EM1) {
            if ((sc.ch == '*' && sc.chPrev != ' ') || IsLineBreak(sc.chNext))
                sc.ForwardSetState(SCE_MARKDOWN_DEFAULT);
        }
        else if (sc.state == SCE_MARKDOWN_EM2) {
            if ((sc.ch == '_' && sc.chPrev != ' ') || IsLineBreak(sc.chNext))
                sc.ForwardSetState(SCE_MARKDOWN_DEFAULT);
        }
        else if (sc.state == SCE_MARKDOWN_CODEBK) {
            if (sc.atLineStart && sc.Match("~~~")) {
                Sci_Position i = 1;
                while (!IsLineBreak(sc.GetRelative(i)) && sc.currentPos + i < endPos)
                    i++;
                sc.Forward(i);
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
        }
        else if (sc.state == SCE_MARKDOWN_STRIKEOUT) {
            if ((sc.Match("~~") && sc.chPrev != ' ') || IsLineBreak(sc.GetRelative(2))) {
                sc.Forward(2);
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
        }
        else if (sc.state == SCE_MARKDOWN_LINE_BEGIN) {
            // Header
            if (sc.Match("######")) {
                SetStateAndZoom(SCE_MARKDOWN_HEADER6, 6, '#', sc);
                freezeCursor = true;
            }
            else if (sc.Match("#####")) {
                SetStateAndZoom(SCE_MARKDOWN_HEADER5, 5, '#', sc);
                freezeCursor = true;
            }
            else if (sc.Match("####")) {
                SetStateAndZoom(SCE_MARKDOWN_HEADER4, 4, '#', sc);           
                freezeCursor = true;
            }
            else if (sc.Match("###")) {
                SetStateAndZoom(SCE_MARKDOWN_HEADER3, 3, '#', sc);
                freezeCursor = true;
            }
            else if (sc.Match("##")) {
                SetStateAndZoom(SCE_MARKDOWN_HEADER2, 2, '#', sc);
                freezeCursor = true;
            }
            else if (sc.Match("#")) {
                // Catch the special case of an unordered list
                if (sc.chNext == '.' && IsASpaceOrTab(sc.GetRelative(2))) {
                    precharCount = 0;
                    sc.SetState(SCE_MARKDOWN_PRECHAR);
                }
                else {
                    SetStateAndZoom(SCE_MARKDOWN_HEADER1, 1, '#', sc);
                    freezeCursor = true;
                }
            }
            // Code block
            else if (sc.Match("~~~")) {
                if (!HasPrevLineContent(sc))
                    sc.SetState(SCE_MARKDOWN_CODEBK);
                else
                    sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
            else if (sc.ch == '=') {
                if (HasPrevLineContent(sc) && FollowToLineEnd('=', SCE_MARKDOWN_HEADER1, endPos, sc))
                    ;
                else
                    sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
            else if (sc.ch == '-') {
                if (HasPrevLineContent(sc) && FollowToLineEnd('-', SCE_MARKDOWN_HEADER2, endPos, sc))
                    ;
                else {
                    precharCount = 0;
                    sc.SetState(SCE_MARKDOWN_PRECHAR);
                }
            }
            else if (sc.atLineEnd) {
                sc.ForwardSetState(SCE_MARKDOWN_LINE_BEGIN);
                freezeCursor = true;
            }
            else {
                precharCount = 0;
                sc.SetState(SCE_MARKDOWN_PRECHAR);
            }
        }

        // The header lasts beyond the newline (eolfilled)
        else if (IsInHeaderState(sc.state)) {
            if (sc.atLineEnd) {
                sc.ForwardSetState(SCE_MARKDOWN_LINE_BEGIN);
                freezeCursor = true;
            }
        }

        // New state only within the initial whitespace
        if (sc.state == SCE_MARKDOWN_PRECHAR) {
            // Blockquote
            if (sc.ch == '>' && precharCount < 5)
                sc.SetState(SCE_MARKDOWN_BLOCKQUOTE);
            /*
            // Begin of code block
            else if (!HasPrevLineContent(sc) && (sc.chPrev == '\t' || precharCount >= 4))
                sc.SetState(SCE_MARKDOWN_CODEBK);
            */
            // HRule - Total of three or more hyphens, asterisks, or underscores
            // on a line by themselves
            else if ((sc.ch == '-' || sc.ch == '*' || sc.ch == '_') && IsValidHrule(endPos, sc))
                ;
            // Unordered list
            else if ((sc.ch == '-' || sc.ch == '*' || sc.ch == '+') && IsASpaceOrTab(sc.chNext)) {
                sc.SetState(SCE_MARKDOWN_ULIST_ITEM);
                sc.ForwardSetState(SCE_MARKDOWN_DEFAULT);
            }
            // Ordered list
            else if (IsADigit(sc.ch)) {
                int digitCount = 0;
                while (IsADigit(sc.GetRelative(++digitCount)))
                    ;
                if (sc.GetRelative(digitCount) == '.' &&
                        IsASpaceOrTab(sc.GetRelative(digitCount + 1))) {
                    sc.SetState(SCE_MARKDOWN_OLIST_ITEM);
                    sc.Forward(digitCount + 1);
                    sc.SetState(SCE_MARKDOWN_DEFAULT);
                }
            }
            // Alternate Ordered list
            else if (sc.ch == '#' && sc.chNext == '.' && IsASpaceOrTab(sc.GetRelative(2))) {
                sc.SetState(SCE_MARKDOWN_OLIST_ITEM);
                sc.Forward(2);
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
            else if (sc.ch != ' ' || precharCount > 2)
                sc.SetState(SCE_MARKDOWN_DEFAULT);
            else
                ++precharCount;
        }

        // Any link
        if (sc.state == SCE_MARKDOWN_LINK) {
            if (sc.Match("](") && sc.GetRelative(-1) != '\\') {
              sc.Forward(2);
              isLinkNameDetecting = true;
            }
            else if (sc.Match("]:") && sc.GetRelative(-1) != '\\') {
              sc.Forward(2);
              sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
            else if (!isLinkNameDetecting && sc.ch == ']' && sc.GetRelative(-1) != '\\') {
              sc.Forward();
              sc.SetState(SCE_MARKDOWN_DEFAULT);
            }
            else if (isLinkNameDetecting && sc.ch == ')' && sc.GetRelative(-1) != '\\') {
              sc.Forward();
              sc.SetState(SCE_MARKDOWN_DEFAULT);
              isLinkNameDetecting = false;
            }
        }

        // New state anywhere in doc
        if (sc.state == SCE_MARKDOWN_DEFAULT) {
            if (sc.atLineStart && sc.ch == '#') {
                sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
                freezeCursor = true;
            }
            // Links and Images
            if (sc.Match("![")) {
              sc.SetState(SCE_MARKDOWN_LINK);
              sc.Forward(1);
            }
            else if (sc.ch == '[' && sc.GetRelative(-1) != '\\') {
              sc.SetState(SCE_MARKDOWN_LINK);
            }
            // Code - also a special case for alternate inside spacing
            else if (sc.Match("``") && sc.GetRelative(3) != ' ' && AtTermStart(sc)) {
                const int openingSpan = (sc.GetRelative(2) == '`') ? 2 : 1;
                sc.SetState(SCE_MARKDOWN_CODE2);
                sc.Forward(openingSpan);
            }
            else if (sc.ch == '`' && sc.chNext != ' ' && IsCompleteStyleRegion(sc, "`")) {
                sc.SetState(SCE_MARKDOWN_CODE);
            }
            // Strong
            else if (sc.Match("**") && sc.GetRelative(2) != ' ' && IsCompleteStyleRegion(sc, "**")) {
                sc.SetState(SCE_MARKDOWN_STRONG1);
                sc.Forward();
           }
            else if (sc.Match("__") && sc.GetRelative(2) != ' ' && IsCompleteStyleRegion(sc, "__")) {
                sc.SetState(SCE_MARKDOWN_STRONG2);
                sc.Forward();
            }
            // Emphasis
            else if (sc.ch == '*' && sc.chNext != ' ' && IsCompleteStyleRegion(sc, "*")) {
                sc.SetState(SCE_MARKDOWN_EM1);
            } else if (sc.ch == '_' && sc.chNext != ' ' && IsCompleteStyleRegion(sc, "_")) {
                sc.SetState(SCE_MARKDOWN_EM2);
            }
            // Strikeout
            else if (sc.Match("~~") && !(sc.GetRelative(2) == '~' || sc.GetRelative(2) == ' ') &&
                IsCompleteStyleRegion(sc, "~~")) {
                sc.SetState(SCE_MARKDOWN_STRIKEOUT);
                sc.Forward();
            }
            // Beginning of line
            else if (IsLineBreak(sc.ch)) {
                sc.SetState(SCE_MARKDOWN_LINE_BEGIN);
            }
        }
        // Advance if not holding back the cursor for this iteration.
        if (!freezeCursor)
            sc.Forward();
        freezeCursor = false;
    }
    sc.Complete();
}

LexerModule lmMarkdown(SCLEX_MARKDOWN, ColorizeMarkdownDoc, "markdown");
