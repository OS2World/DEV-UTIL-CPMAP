CPMAP (Codepage Map)
| Version 1.2, 2011-03-30

| CPMAP is a Presentation Manager program for displaying OS/2 codepage tables
| in a selectable font.  It is also capable of showing detailed information 
| about the current codepage and/or font.

| CPMAP can be useful for various purposes:
|   - Viewing or analyzing codepages.
|   - Viewing, analyzing, or debugging fonts (and font drivers).
|   - As a demonstration of how to render and manipulate Unicode text under 
|     OS/2 Presentation Manager/GPI, including various useful techniques and
|     workarounds.
|
| CPMAP is _not_ a character map in the usual sense: it does not have any kind
| of clipboard support for copying characters.  It is primarily a developer
| tool.


ABOUT CODEPAGES

  A codepage is an encoding table that translates byte values into readable 
  characters.  OS/2 uses the currently-active codepage to determine what 
  character a given byte value represents.

  Under OS/2, codepages can be used in one of three different ways.  First,
  certain codepages may be designated as the system (or process) codepage,
  through the CONFIG.SYS "CODEPAGE" setting.  Second, many codepages are
  available for use as a display (PM) codepage within Presentation Manager
  applications.  Third, any codepage known to OS/2 may be used as a translation
  table (in conjunction with the conversion functions of the Unicode API) to
  convert data from one codepage to another.

  All codepages known to OS/2 are defined in the \LANGUAGE\CODEPAGE directory
  on the system volume.  With the exception of the alias table UCSTBL.LST,
  every file in this directory defines a single codepage.  UCSTBL.LST is a text
  file that defines aliases (alternate names) for various codepages.  Together,
  the codepage files and defined aliases make up the list of codepages which
  OS/2 can use.

  CPMAP is a graphical program that can display a complete character map of any
  of these codepages (even those which are not available for use as system or
  PM codepages).


REQUIREMENTS

  CPMAP should run on OS/2 Warp 3 with FixPak 32 and up, OS/2 Warp 4 with
  FixPak 6 and up, and any version of Warp Server for e-business, the
  Convenience Paks and eComStation.

| A TrueType font with Unicode support (that is, with a Unicode cmap table;
| virtually all TrueType and OpenType fonts released in the past 15 years 
| have this) is generally recommended for best results. 

  In the case of Warp 3 or Warp 4, the Java 1.1.8 runtime environment with
  Unicode font is a recommended install (it adds some improvements to the
  OS/2 Unicode support).

  You are recommended to have a UCONV.DLL with a BLDLEVEL of 14.83 or later,
  otherwise you may encounter problems after running CPMAP a large number of
  times (due to a resource leak in earlier versions).  This file should be
  included in Warp 4 FixPak 17 and Convenience Pak FixPak 3; those users
  without access to these fixes should search for the single-file fix that was
  made available for a while through the Innotek support forums.

  You are also recommended to have a PMMERGE.DLL with a BLDLEVEL of 14.106 or
  later, to avoid a problem with system-wide text corruption that was triggered
  by switching codepages a large number of times.  This file corresponds to IBM
  fix PJ31908 (available to eComStation users).


SYNTAX

  Usage: CPMAP [ /cpXXXX ]  [ /u ]

  Parameters:
      /cpXXXX   Display codepage number XXXX on startup (instead of the current
                codepage).
      /u        Display UCS values on startup (instead of glyphs).


USING CPMAP

  The CPMAP user interface displays a simple 16x16 cell character grid, each of
  which corresponds to a byte value (codepoint) under the codepage being
  displayed.

  Normally, each cell displays the glyph corresponding to the character value
  assigned to that codepoint, as displayed under the current font.  If you
  specified the /u parameter on startup, or select "Show UCS values" from the
  Options menu, the cells will contain the Unicode (UCS) character code for the
  character instead.

  Coloured cells, or those containing certain special symbols, have special
  meanings which are described below:

  Blank Cell:
    A (white) cell which appears to be empty indicates one of the following:
     - A character with no visible components (which may occasionally depend on
       the current font): this is typical of characters like "space" and its
       variants, as well as the NULL character.
     - If the current font does not contain the "no-character" glyph, then it
       may also indicate an unassigned or non-displayable value (see next item).

  No-Character Glyph:
    The no-character glyph, which usually resembles a large square outline
    (although it depends on the current font) is used to indicate a character
    which cannot be displayed, normally for one of the following reasons:
     - The current font does not contain a glyph for the character; this is
       probably the most common reason.
     - The character has an assigned but indeterminate value (this applies to
       codepoints which are designated as "user-defined" characters).
     - The character is inherently non-displayable (this applies to control
       bytes such as "tab", "form feed", "carriage return", "bell", etc.).

  Unicode Substitution Glyph:
    This normally appears as a white question mark within a black diamond, and
    is used to indicate a codepoint which has no character value assigned to
    it.  (Under certain multibyte codepages, if the Unicode APIs failed to
    correctly report the range of illegal byte values, it may also represent
    codepoints which cannot be used to represent valid characters at all.)
    It also represents the literal character U+FFFD (the Unicode "substitution"
    glyph), which may be seen under codepages 1200 (0xFFFD), 1207 (0xFFFD), and
    1208 (0xEFBFBD).

  Blue, Green, or Cyan Background:
    A cell which is filled in with one of these colours (normally containing
    a grey "ideographic half fill" character, although this depends on the
    display font supporting it) indicates part of a multibyte character.  That
    is, the codepoint can only represent a valid character when combined with
    additional byte values after the current one.
     - Blue indicates that one more byte must follow to form a valid character.
     - Green indicates that two more bytes must follow.
     - Cyan indicates that three more bytes must follow.
    See the section "multibyte characters" (below) for more information.

  Magenta Background:
    A dark magenta-coloured cell is used to indicate a UCS "surrogate" value.
    Surrogates are reserved codepoints within Unicode that are not characters
    at all, but a special type of control marker used by certain encodings.

  Red Background:
    This indicates that codepage conversion failed in some way.  Bright red
    indicates a failure to generate the UCS-2 value; dark red indicates a
    failure to convert from UCS-2 to PM Unicode format (UPF-8).  If these
    suddenly start occurring throughout CPMAP, it may indicate an exhaustion
    of system resources within UCONV.DLL (versions prior to OS/2 Warp 4 FixPak
    17 and Convenience Package FixPak 3 were vulnerable to this problem), and
    a reboot is likely necessary.

  Grey Background:
    A blank grey cell indicates a codepoint which is not merely unassigned, but
    which, according to the codepage specification, cannot legally represent a
    character value at all.  (This depends on the codepage correctly reporting
    such values to the Unicode API; some multibyte codepages notably fail to do
    so, in which case these codepoints will most likely display the Unicode
    substitution glyph instead.)

| The overall window background colour is normally white.  In the event that a 
| non-Unicode font is being used (see below under "Font Considerations"), the
| window background will be cream-coloured (pale yellow) instead.  This 
| indicates that codepage rather than Unicode rendering is in effect.

  You can change the current codepage by selecting "Codepage", "Select" from 
  the menubar.  On the dialog that appears, you can type the new codepage (by
  number) manually, or choose from a list of codepages detected on your system.

  You can also change the display font by selecting "Font" from the "Options" 
  menu.  Since not all fonts obviously support all possible characters, the 
  range of characters that are correctly displayed will depend on the font in 
  use.  See the section entitled "Font Considerations" (below) for additional
  information.


Multibyte Characters

  Byte values which represent leading parts of multibyte characters are shown
  as coloured cells (blue for a two-byte character, green for a three-byte
  character, and cyan for a four-byte character).  If the current font supports
  it, these cells will also contain a light grey "ideographic half fill" glyph,
  which (under most fonts) normally resembles a rectangular box containing a
  pair of crossed diagonal lines.

  Clicking on such a cell with the mouse will cause all possible byte values
  UNDER that codepoint to be displayed.  (You can click on any area outside the
  character grid, or press the Escape key, to restore the character grid to its
  previous state.)

  The same principle holds true when the grid is displaying secondary byte
  values: if additional following bytes are required to represent a valid
  character, the cells will again be coloured accordingly (blue if one more
  byte is required, green if two more are required) and you can click on a
  cell to display the codepoints underneath.


FONT CONSIDERATIONS

| There are a few limitations regarding font support.
|
| CPMAP normally tries to render text using Unicode.  This depends, however on
| the font being seen as a Unicode font.  This is a factor not only of the font
| itself, but also of the font driver.  Basically, the font must not only have
| a Unicode index table (cmap), but the OS/2 font driver responsible for that
| font must do both of the following:
|
|   - Pass the font to Presentation Manager using the UNICODE glyphlist.  The
|     IBM TrueType driver will only do this for Unicode fonts with at least
|     1,024 glyphs.  The FreeType/2 replacement TrueType driver uses somewhat
|     different logic for this depending on version; since version 1.2 its
|     behaviour has been configurable.
|
|   - Ensure that the fsType field of the FONTMETRICS structure for that font
|     has the Unicode flag (0x40) set.  (The reason for this is that CPMAP uses
|     this flag to identify whether a font uses Unicode or not.)  Note that 
|     some older versions of FreeType/2 do not set this flag correctly, but the
|     latest release (version 1.3x at the time of writing) should be fine.
|
| It should be noted that, at the present time, ONLY TrueType fonts are capable
| of being seen as Unicode fonts.  Bitmap fonts cannot be Unicode fonts, as 
| they use their own specialized format.  Type 1 (PostScript/ATM) fonts also
| cannot use Unicode, due to the limitations of the current OS/2 Type 1 driver.
|
| When a non-Unicode font (including all Type 1 and/or bitmap fonts) is in use,
| CPMAP will fall back to using codepage-specific text rendering.  In this 
| case, only non-Unicode codepages which are directly supported by Presentation
| Manager as display codepages will be viewable. 


HISTORY

| 1.2 (2011-03-30)
|  - Non-Unicode fonts will now automatically use codepage rendering instead
|    of Unicode rendering.  This has some limitations: it only works with 
|    PM-compatible codepages, and the Unicode codepages (1200/1207/1208) will
|    not be available.  Also, this depends on the font driver correctly 
|    reporting (via FONTMETRICS fsType) that fonts actually are Unicode-capable
|    or not (some old versions of FreeType/2 do not do this).
|  - Added a font information dialog to display the current font metrics.
|  - In most cases, the font selector should now default to the correct
|    (current) style name, instead of the first one in alphabetical order 
|    (usually bold) like before.
|  - Added OS2UGL (Extended Universal Glyph List, actually works now) to the
|    codepage selector; may also be selected by specifying codepage "1".
|    Ironically requires a Unicode font to display correctly (OS2UGL is not 
|    supported as a display codepage, so Unicode rendering must be used).
|  - UTF-8 (codepage 1208) no longer shows invalid codepoints as characters.
|  - Various menu commands now have keyboard shortcuts.

  1.1 (2007-07-10)
   - The current font is now displayed in the program title
   - Fixed case where detection of codepage files would fail if %ULSPATH%
     had a trailing semicolon.
   - Fixed poor vertical text alignment in three- and four-byte wards.

  1.0 (2007-07-06)
   - Codepage selection dialog now offers a list of all installed codepages.
   - Better positioning and clipping of character cell contents.
   - Code cleanup.

  0.9 (2007-04-04)
   - Some adjustments to the text autosize/positioning logic.
   - Raster (bitmap) fonts are now supported.

  0.8 (2007-03-26)
   - Text now dynamically resizes to fit current window size.
   - Double-byte conversion now uses UniUconv* functions; some codepages
     (e.g. 955) now display correctly as a result.
   - A few minor cosmetic changes.

  0.7 (2007-01-26)
   - First public release.
   - A couple of minor bugfixes.

  0.6 (2006-11-13)
   - Limited release (Warpstock 2006).


LICENSE

  CPMAP is (C) 2006-2011 Alex Taylor.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

   1. Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
   2. Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
   3. The name of the author may not be used to endorse or promote products
      derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
  EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE.

