/*
 * CPMAP.C (C) 2007 Alex Taylor
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  1. Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *  3. The name of the author may not be used to endorse or promote products
 *     derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO
 * EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
 * IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define INCL_GPI
#define INCL_WIN
#define INCL_DOSFILEMGR
#define INCL_DOSMISC
#include <os2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uconv.h>
#include "ids.h"


// ---------------------------------------------------------------------------
// MACROS
#define ErrorPopup( text ) \
    WinMessageBox( HWND_DESKTOP, HWND_DESKTOP, text, "Error", 0, MB_OK | MB_ERROR )

#define UNICFROM2BYTES( b1, b2 )    (( b1 << 8 ) | b2 )
#define BYTE1FROMUNIC( unic )        ( unic >> 8 )
#define BYTE2FROMUNIC( unic )        ( unic & 0x00FF )
#define ISLEADINGBYTE( b1 )         (( b1 >= 2 && b1 <= 4 ) ? TRUE : FALSE )


// ---------------------------------------------------------------------------
// CONSTANTS

// max. number of codepages to read into selection list
#define US_CPLIST_MAX   512

// fByteType constants
#define ULS_SBCHAR      1       // single-byte character
#define ULS_2BYTE_LEAD  2       // first byte of a 2-byte character
#define ULS_3BYTE_LEAD  3       // first byte of a 3-byte character
#define ULS_4BYTE_LEAD  4       // first byte of a 4-byte character
#define ULS_CODEPOINT   255     // "codepoint" (PROBABLY indicates "illegal character value")

// Codepage value flags (for flValues*)
#define CPV_1_MORE      0x0001  // 1 byte follows this one to form a valid character
#define CPV_2_MORE      0x0002  // 2 bytes follow this one to form a valid character
#define CPV_3_MORE      0x0004  // 3 bytes follow this one to form a valid character
#define CPV_CODEPOINT   0x0008  // unusable codepoint
#define CPV_SURROGATE   0x0020  // UCS surrogate value
#define CPV_CONTROL     0x0040  // control byte
#define CPV_UNKNOWN     0x0080  // OS/2 reported an unknown byte type
#define CPV_UCS_FAILED  0x0100  // conversion from codepage to UCS-2 failed
#define CPV_UPF_FAILED  0x0200  // conversion from UCS-2 to UPF-8 failed

// string length constants
#define US_TITLE_MAX    100 + FACESIZE  // program window title
#define US_ERRTEXT_MAX  255             // an error string
#define US_UCSTBL_MAX   255             // a line in UCSTBL.LST
#define US_CPNAME_MAX   12              // a codepage name
#define US_CPSPEC_MAX   64              // a codepage conversion specifier
#define US_SZOUT_MAX    6               // a null-terminated UPF-8 character
#define US_FMITEM_MAX   100             // a FONTMETRICS item description
#define US_FMVAL_MAX    100             // a FONTMETRICS item value
#define US_FMDESC_MAX   255             // a FONTMETRICS item explanation

/* Number of records in the font information container.  Derived from 52 fields
 * in FONTMETRICS, ten of which are paired together.
 */
#define US_FM_RECORDS   47

/* Length of the UniChar buffer provided for converting a single codepage
 * character to a ULS codepoint.  Normally the API should generate a single
 * UniChar value (plus a terminating NULL), but with some codepages it seems
 * to insist on appending an extra (garbage?) UniChar.  So we allow three
 * characters (2 + NULL) to avoid a ULS_BUFFERFULL error from UniUconvToUcs().
 * We'll only actually use the first element.
 */
#define UCS_OUT_LEN     3

// mode flags
#define CH_MODE_SINGLE  1   // indicates single- (or lead-) byte output mode
#define CH_MODE_DOUBLE  2   // indicates two-byte output mode
#define CH_MODE_TRIPLE  3   // indicates three-byte output mode
#define CH_MODE_QUADRPL 4   // indicates four-byte output mode

// default font
#define SZ_DEFAULTFONT  "Times New Roman MT 30"


// ---------------------------------------------------------------------------
// TYPEDEFS

// record structure for the current-font information container
typedef struct _font_info_record {
    MINIRECORDCORE record;               // standard data
    PSZ            pszItemName,          // content of item name field
                   pszItemValue,         // content of item value field
                   pszItemDetails;       // content of item explanation field
} FIRECORD, *PFIRECORD;

// Data about the current codepage.
typedef struct _cp_globals {
    uconv_attribute_t attributes;        // attributes of the current codepage
    udcrange_t        user_range[ 32 ];  // user-defined character ranges
    CHAR              fByteType[ 256 ],  // array of (leading) codepoint types
                      fFollowing[ 256 ]; // array indicating valid secondary bytes
    ULONG             flValues1[ 256 ],  // special flags for 1-byte codepoints
                      flValues2[ 256 ],  // special flags for 2-byte codepoints
                      flValues3[ 256 ],  // special flags for 3-byte codepoints
                      flValues4[ 256 ];  // special flags for 4-byte codepoints
} CPGLOBAL, *PCPGLOBAL;


// Common GPI positioning coordinates.
typedef struct _pos_globals {
    LONG  xMargin,                       // horizontal margin around the character grid
          yMargin,                       // vertical margin around the character grid
          xHeadWidth,                    // width of a row heading
          yHeadHeight,                   // height of a column heading
          xCellWidth,                    // width of a character cell
          yCellHeight;                   // height of a character cell
} GPIGLOBAL, *PGPIGLOBAL;


// General program data.
typedef struct _app_globals {
    HAB     hab;                              // anchor block handle
    HMQ     hmq;                              // message-queue handle
    // The following items identify the current program state:
    CHAR    szFont[ FACESIZE ],               // the current font (family name)
            szFace[ FACESIZE ];               // the current font (face name)
    BYTE    fCharMode;                        // indicates single, double, or triple-byte ouput mode
    BOOL    fUCS,                             // show UCS values or glyphs?
            fRaster,                          // are we currently using a raster font?
            fUnicode;                         // are we currently using a Unicode-capable font?
    // The following items are ULS codepage conversion objects:
    UconvObject uconvCP,                      // used to convert codepoints to UCS
                uconvUPF;                     // used to convert UCS to UPF-8 for output
    // The following items describe characters in the current codepage:
    ULONG   ulCP;                             // the current codepage being viewed
    UCHAR   szOutput1[ 256 ][ US_SZOUT_MAX ], // converted single-byte characters for output
            szOutput2[ 256 ][ US_SZOUT_MAX ], // converted two-byte characters for output
            szOutput3[ 256 ][ US_SZOUT_MAX ], // converted three-byte characters for output
            szOutput4[ 256 ][ US_SZOUT_MAX ], // converted four-byte characters for output
            ucFirst,                          // current leading byte for multi-byte characters
            ucSecond,                         // current second byte for three-byte characters
            ucThird;                          // current third byte for four-byte characters
} APPGLOBAL, *PAPPGLOBAL;


// ---------------------------------------------------------------------------
// FUNCTIONS

MRESULT EXPENTRY ClientWndProc( HWND, ULONG, MPARAM, MPARAM );
MRESULT EXPENTRY CodepageDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 );
MRESULT EXPENTRY InfoDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 );
MRESULT EXPENTRY MetricsDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 );
MRESULT EXPENTRY AboutDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 );
int              QSCmpCP( const void *key, const void *element );
void             CentreWindow( HWND hwnd );
void             SelectFont( HWND hwnd );
BOOL             GetImageFont( HPS hps, PSZ pszFontFace, PFATTRS pfAttrs, LONG lCY );
void             SelectCodepage( HWND hwnd );
BOOL             CheckSupportedCodepage( HWND hwnd, ULONG ulCP );
BOOL             SetNewCodepage( HWND hwnd, ULONG ulCP );
ULONG            SetupConversions( ULONG ulCP );
ULONG            GenerateOutput( void );
ULONG            GenerateOutputLvl2( UCHAR ucLeadByte );
ULONG            GenerateOutputLvl3( UCHAR ucFirst, UCHAR ucSecond );
ULONG            GenerateOutputLvl4( UCHAR ucFirst, UCHAR ucSecond, UCHAR ucThird );
MRESULT          PaintClient( HWND );
void             PaintChars1B( HPS hps, RECTL rcl, LONG yTextOffset, BOOL fUnicode );
void             PaintChars2B( HPS hps, RECTL rcl, LONG yTextOffset, BOOL fUnicode );
void             PaintChars3B( HPS hps, RECTL rcl, LONG yTextOffset, BOOL fUnicode );
void             PaintChars4B( HPS hps, RECTL rcl, LONG yTextOffset, BOOL fUnicode );
void             PopulateFontDetails( HWND hwndCnr );
void             ProcessClick( HWND hwnd, SHORT ptx, SHORT pty );


// ---------------------------------------------------------------------------
// GLOBALS

CPGLOBAL  codepage;
GPIGLOBAL coord;
APPGLOBAL global;


/* ------------------------------------------------------------------------- *
 * Main program                                                              *
 * ------------------------------------------------------------------------- */
int main( int argc, char *argv[] )
{
    HAB     hab;                            // anchor block handle
    HMQ     hmq;                            // message queue handle
    HWND    hwndFrame,                      // frame window handle
            hwndClient;                     // client window handle
    QMSG    qmsg;                           // message queue
    CHAR    szClass[] = "Codepage Viewer",  // class name
            szError[ US_ERRTEXT_MAX ],      // error message buffer
            szTitle[ US_TITLE_MAX ];        // window title
    LONG    scr_cx, scr_cy,                 // screen size coordinates
            win_cx, win_cy;                 // initial window size
    ULONG   ulRC,                           // return code
            flFrameOpts = FCF_STANDARD & ~FCF_SHELLPOSITION;
    int     a;                              // argument counter


    global.ulCP      = 0;
    global.fUCS      = FALSE;
    global.fUnicode  = TRUE;
    global.uconvCP   = NULL;
    global.uconvUPF  = NULL;
    global.fCharMode = CH_MODE_SINGLE;

    // Read the command-line arguments
    if ( argc > 1 ) {
        for ( a = 1; a < argc; a++ ) {
            strupr( argv[a] );
            if ( strncmp( argv[a], "/CP", 3 ) == 0 ) {
                if ( sscanf( argv[a], "/CP%d", &(global.ulCP) ) == 0 )
                    global.ulCP = 0;
            }
            else if ( strncmp( argv[a], "/U", 2 ) == 0 ) {
                global.fUCS = TRUE;
            }
        }
    }

    strncpy( global.szFont, SZ_DEFAULTFONT, strlen(SZ_DEFAULTFONT) );
    strncpy( global.szFace, SZ_DEFAULTFONT, strlen(SZ_DEFAULTFONT) );

    hab = WinInitialize( 0 );
    hmq = WinCreateMsgQueue( hab, 0 );

    if ( hab && hmq ) {

        global.hab = hab;
        global.hmq = hmq;

        // Create a conversion object for Unicode output under PM
        ulRC = UniCreateUconvObject( (UniChar *) L"IBM-1207@map=display", &(global.uconvUPF) );
        if ( ulRC != ULS_SUCCESS ) {
            sprintf( szError, "Failed to set up UPF-8 conversion for output.\n\nUniCreateUconvObject() returned 0x%X\n", ulRC );
            ErrorPopup( szError );
        } else {

            if ( !global.ulCP ) global.ulCP = WinQueryCp( hmq );
            if ( global.ulCP == 1 )
                sprintf( szTitle, "Viewing OS2UGL - %s", global.szFace );
            else
                sprintf( szTitle, "Viewing Codepage %u - %s", global.ulCP, global.szFace );

            WinRegisterClass( hab, szClass, ClientWndProc, CS_SIZEREDRAW, 0 );
            hwndFrame = WinCreateStdWindow( HWND_DESKTOP, WS_VISIBLE, &flFrameOpts, szClass,
                                            szTitle, 0L, 0, ID_MAINPROGRAM, &hwndClient );

            win_cx = 478;
            win_cy = 472;
            scr_cx = WinQuerySysValue( HWND_DESKTOP, SV_CXSCREEN );
            scr_cy = WinQuerySysValue( HWND_DESKTOP, SV_CYSCREEN );
            WinSetWindowPos( hwndFrame, 0, (scr_cx/2)-(win_cx/2), (scr_cy/2)-(win_cy/2),
                             win_cx, win_cy, SWP_MOVE | SWP_ACTIVATE | SWP_SIZE );

            if ( global.fUCS )
                WinSendMsg( WinWindowFromID(hwndFrame, FID_MENU), MM_SETITEMATTR,
                            MPFROM2SHORT(ID_UCS, TRUE), MPFROM2SHORT(MIA_CHECKED, MIA_CHECKED) );

            while ( WinGetMsg( hab, &qmsg, 0, 0, 0 )) WinDispatchMsg( hab, &qmsg );

            UniFreeUconvObject( global.uconvCP );
            UniFreeUconvObject( global.uconvUPF );
            WinDestroyWindow( hwndFrame );

        }
        WinDestroyMsgQueue( hmq );
        WinTerminate( hab );
    }

    return ( 0 );
}


/* ------------------------------------------------------------------------- *
 * ClientWndProc                                                             *
 *                                                                           *
 * The main application (client) window procedure.                           *
 * ------------------------------------------------------------------------- */
MRESULT EXPENTRY ClientWndProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    HWND    hFrame,
            hMenu;
    PPOINTS ppts;
    USHORT  usChecked;

    switch( msg ) {

        case WM_CREATE:
            memset( global.szOutput1, 0, sizeof(global.szOutput1) );

            // Set up the codepage conversion objects
            if ( SetupConversions( global.ulCP ) != ULS_SUCCESS ) break;

            // Create the array of converted character values
            GenerateOutput();
            break;


        case WM_COMMAND:
            switch( SHORT1FROMMP( mp1 )) {

                case ID_ABOUT:                  // Product information dialog
                    WinDlgBox( HWND_DESKTOP, hwnd, (PFNWP) AboutDlgProc, 0, IDD_ABOUT, NULL );
                    break;

                case ID_BACK:                   // <Esc> pressed (from acceltable)
                    ProcessClick( hwnd, 0, 0 );   // simulate clicking outside grid
                    break;

                case ID_CODEPAGE:               // Select codepage
                    SelectCodepage( hwnd );
                    break;

                case ID_FONT:                   // Font selection dialog
                    SelectFont( hwnd );
                    break;

                case ID_FONTMETRICS:            // Font information dialog
                    WinDlgBox( HWND_DESKTOP, hwnd, (PFNWP) MetricsDlgProc, 0, IDD_METRICS, NULL );
                    break;

                case ID_INFO:                   // Codepage information dialog
                    WinDlgBox( HWND_DESKTOP, hwnd, (PFNWP) InfoDlgProc, 0, IDD_CPINFO, NULL );
                    break;

                case ID_UCS:                    // Show UCS values
                    hFrame = WinQueryWindow( hwnd, QW_PARENT );
                    hMenu  = WinWindowFromID( hFrame, FID_MENU );
                    global.fUCS = !(global.fUCS);
                    usChecked = global.fUCS ? MIA_CHECKED : 0;
                    WinSendMsg( hMenu, MM_SETITEMATTR,
                                MPFROM2SHORT(ID_UCS, TRUE), MPFROM2SHORT(MIA_CHECKED, usChecked) );
                    // Re-generate the output strings for every active level
                    switch ( global.fCharMode ) {
                        case CH_MODE_QUADRPL:
                            memset( global.szOutput4, 0, sizeof(global.szOutput4) );
                            GenerateOutputLvl4( global.ucFirst, global.ucSecond, global.ucThird );
                        case CH_MODE_TRIPLE:
                            memset( global.szOutput3, 0, sizeof(global.szOutput3) );
                            GenerateOutputLvl3( global.ucFirst, global.ucSecond );
                        case CH_MODE_DOUBLE:
                            memset( global.szOutput2, 0, sizeof(global.szOutput2) );
                            GenerateOutputLvl2( global.ucFirst );
                        case CH_MODE_SINGLE:
                            memset( global.szOutput1, 0, sizeof(global.szOutput1) );
                            GenerateOutput();
                        default: break;
                    }
                    WinInvalidateRegion( hwnd, NULLHANDLE, TRUE );
                    break;

                case ID_QUIT:                   // Exit the program
                    WinPostMsg( hwnd, WM_CLOSE, 0, 0 );
                    return (MRESULT) 0;

                default: break;

            }
            return (MRESULT) 0;


        case WM_BUTTON1CLICK:
            ppts = (PPOINTS) &mp1;
            ProcessClick( hwnd, ppts->x, ppts->y );
            return (MRESULT) 0;


        case WM_PAINT:
            PaintClient( hwnd );
            break;

    }

    return WinDefWindowProc( hwnd, msg, mp1, mp2 );
}


/* ------------------------------------------------------------------------- *
 * CodepageDlgProc                                                           *
 *                                                                           *
 * Dialog procedure for the codepage selection dialog.                       *
 * ------------------------------------------------------------------------- */
MRESULT EXPENTRY CodepageDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    static PULONG pulValue;                   // currently-selected codepage
    CHAR          szCPs[ CCHMAXPATH + 1 ],    // location of codepage definitions
                  szCPMask[ CCHMAXPATH + 1 ], // find mask for codepage files
                  szBuf[ US_UCSTBL_MAX + 1 ], // read buffer for codepage aliases
                  szCP[ US_CPNAME_MAX + 1 ];  // codepage name
    ULONG         flFindAttr,                 // find attributes
                  ulCount,                    // number of codepages queried
                  ulTotal,                    // total number of codepages
                  ulCPNum,                    // parsed codepage number
                  ulOrd,                      // ordinal from DosQuerySysInfo
                  codepages[ US_CPLIST_MAX ]; // list of installed codepages
    PSZ           pszEnv,                     // returned environment variable
                  pszTok;                     // tokenized string
    HDIR          hdFind;                     // find operation handle
    FILEFINDBUF3  ffInfo = {0};               // file find info buffer
    SHORT         sIdx;                       // list item index
    FILE          *ucstbl;                    // UCS alias table
    int           i;
    APIRET        rc;


    switch ( msg ) {

        case WM_INITDLG:

            // Get the path to the codepage files (e.g. ?:\LANGUAGE\CODEPAGE)
            if (( DosScanEnv("ULSPATH", &pszEnv ) == NO_ERROR ) && ( strlen(pszEnv) > 0 )) {
                strncpy( szCPs, pszEnv, CCHMAXPATH );
                i = strlen( szCPs ) - 1;
                if ( szCPs[ i ] == ';') szCPs[ i ] = '\0';
            } else {
                rc = DosQuerySysInfo( QSV_BOOT_DRIVE, QSV_BOOT_DRIVE, &ulOrd, sizeof(ULONG) );
                if (( rc != NO_ERROR ) || ( ulOrd > 26 ) || ( ulOrd < 1 ))
                    ulOrd = 3;
                sprintf( szCPs, "%c:\\LANGUAGE", 64 + ulOrd );
            }
            strncat( szCPs, "\\CODEPAGE", CCHMAXPATH );

            // Now find all installed codepage files
            strcpy( szCPMask, szCPs );
            strncat( szCPMask, "\\IBM*", CCHMAXPATH );
            ulTotal = 0;
            memset( &codepages, 0, sizeof(codepages) );
            hdFind     = HDIR_SYSTEM;
            flFindAttr = FILE_NORMAL | FILE_SYSTEM | FILE_HIDDEN | FILE_READONLY;
            ulCount    = 1;
            rc = DosFindFirst( szCPMask, &hdFind, flFindAttr, &ffInfo,
                               sizeof(ffInfo), &ulCount, FIL_STANDARD );
            while ( rc == NO_ERROR ) {
                strupr( ffInfo.achName );
                if ( sscanf( ffInfo.achName, "IBM%u", &ulCPNum ))
                    codepages[ ulTotal++ ] = ulCPNum;
                ulCount = 1;
                rc = DosFindNext( hdFind, &ffInfo, sizeof(ffInfo), &ulCount );
            }
            DosFindClose( hdFind );

            // Now add any numeric aliases found in UCSTBL.LST
            strncat( szCPs, "\\UCSTBL.LST", CCHMAXPATH );
            ucstbl = fopen( szCPs, "r");
            if ( ucstbl != NULL ) {
                while ( ! feof(ucstbl) ) {
                    ulCPNum = 0;
                    if ( fgets( szBuf, US_UCSTBL_MAX, ucstbl ) == NULL ) break;
                    if (( strlen(szBuf) == 0 ) || ( szBuf[0] == '#')) continue;
                    if (( pszTok = strtok( szBuf, " \t")) == NULL ) continue;
                    if (( sscanf( pszTok, "IBM-%u", &ulCPNum )) ||
                        ( sscanf( pszTok, "IBM%u", &ulCPNum )))   {
                        // (make sure it isn't already in the list)
                        for ( i = 0; i < ulTotal; i++ ) {
                            if ( codepages[ i ] == ulCPNum ) {
                                ulCPNum = 0;
                                break;
                            }
                        }
                        if ( ulCPNum ) codepages[ ulTotal++ ] = ulCPNum;
                    }
                }
                fclose( ucstbl );
            }

            // Sort the codepages by number and populate the codepage list
            qsort( codepages, ulTotal, sizeof(ULONG), QSCmpCP );
            ulTotal = 0;
            while ( codepages[ ulTotal ] ) {
                sprintf( szCP, "%d", codepages[ ulTotal++ ] );
                WinSendDlgItemMsg( hwnd, IDD_CPSELECT, LM_INSERTITEM,
                                   MPFROMSHORT(LIT_NONE), MPFROMP(szCP) );
            }
            // Add the special OS2UGL encoding
            WinSendDlgItemMsg( hwnd, IDD_CPSELECT, LM_INSERTITEM,
                                   MPFROMSHORT(LIT_END), MPFROMP("OS2UGL") );

            // Get the currently-selected codepage number
            pulValue = (PULONG) mp2;
            if ( *pulValue == 1 )
                strcpy( szCP, "OS2UGL");
            else
                sprintf( szCP, "%d", *pulValue );
            WinSetDlgItemText( hwnd, IDD_CPSELECT, szCP );

            sIdx = (SHORT) WinSendDlgItemMsg( hwnd, IDD_CPSELECT,
                                              LM_SEARCHSTRING,
                                              MPFROM2SHORT(0, LIT_FIRST),
                                              MPFROMP(szCP)              );
            if ( sIdx != LIT_ERROR && sIdx != LIT_NONE )
                WinSendDlgItemMsg( hwnd, IDD_CPSELECT, LM_SELECTITEM,
                                   MPFROMSHORT(sIdx), MPFROMSHORT(TRUE) );
            CentreWindow( hwnd );
            break;


        case WM_COMMAND:
            switch( SHORT1FROMMP( mp1 )) {
                case DID_OK:
                    if (( ! WinQueryDlgItemText( hwnd, IDD_CPSELECT, sizeof(szCP), szCP )) ||
                        ( ! sscanf( szCP, "%d", pulValue )))
                    {
                        if ( ! stricmp( szCP, "OS2UGL"))
                            *pulValue = 1;
                        else {
                            ErrorPopup("Invalid codepage number.");
                            return (MRESULT) 0;
                        }
                    }
                    break;

                default: break;
            }
            break;


        default: break;
    }

    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}


/* ------------------------------------------------------------------------- *
 * InfoDlgProc                                                               *
 *                                                                           *
 * Dialog procedure for the codepage information dialog.                     *
 * ------------------------------------------------------------------------- */
MRESULT EXPENTRY InfoDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    CHAR  szTitle[ US_TITLE_MAX ],
          szByteRange[ 12 ],
          szEncoding[ 32 ],
          szSubChar[ 12 ],
          szBuf[ 6 ],
          szListItem[ 12 ];
    int   i;


    switch ( msg ) {

        case WM_INITDLG:
            sprintf( szTitle, "Codepage %d - Information", global.ulCP );
            WinSetWindowText( hwnd, szTitle );

            if ( codepage.attributes.mb_min_len != codepage.attributes.mb_max_len )
                sprintf( szByteRange, "%d-%d", codepage.attributes.mb_min_len, codepage.attributes.mb_max_len );
            else
                sprintf( szByteRange, "%d", codepage.attributes.mb_min_len );
            WinSetDlgItemText( hwnd, IDD_CHARBYTES, szByteRange );

            switch ( codepage.attributes.esid ) {
                case ESID_sbcs_data:    sprintf( szEncoding, "SBCS-DATA (0x%X)", codepage.attributes.esid );      break;
                case ESID_sbcs_pc:      sprintf( szEncoding, "SBCS-PC (0x%X)", codepage.attributes.esid );        break;
                case ESID_sbcs_ebcdic:  sprintf( szEncoding, "SBCS-HOST (0x%X)", codepage.attributes.esid );      break;
                case ESID_sbcs_iso:     sprintf( szEncoding, "SBCS-ISO (0x%X)", codepage.attributes.esid );       break;
                case ESID_sbcs_windows: sprintf( szEncoding, "SBCS-Windows (0x%X)", codepage.attributes.esid );   break;
                case ESID_sbcs_alt:     sprintf( szEncoding, "SBCS-Alternate (0x%X)", codepage.attributes.esid ); break;
                case ESID_dbcs_data:    sprintf( szEncoding, "DBCS-DATA (0x%X)", codepage.attributes.esid );      break;
                case ESID_dbcs_pc:      sprintf( szEncoding, "DBCS-PC (0x%X)", codepage.attributes.esid );        break;
                case ESID_dbcs_ebcdic:  sprintf( szEncoding, "DBCS-HOST (0x%X)", codepage.attributes.esid );      break;
                case ESID_mbcs_data:    sprintf( szEncoding, "MBCS-DATA (0x%X)", codepage.attributes.esid );      break;
                case ESID_mbcs_pc:      sprintf( szEncoding, "MBCS-PC (0x%X)", codepage.attributes.esid );        break;
                case ESID_mbcs_ebcdic:  sprintf( szEncoding, "MBCS-HOST (0x%X)", codepage.attributes.esid );      break;
                case ESID_ucs_2:        sprintf( szEncoding, "UCS-2 (0x%X)", codepage.attributes.esid );          break;
                case ESID_ugl:          sprintf( szEncoding, "UGL (0x%X)", codepage.attributes.esid );            break;
                case ESID_utf_8:        sprintf( szEncoding, "UTF-8 (0x%X)", codepage.attributes.esid );          break;
                case ESID_upf_8:        sprintf( szEncoding, "UPF-8 (0x%X)", codepage.attributes.esid );          break;
                default:                sprintf( szEncoding, "(unknown) (0x%X)", codepage.attributes.esid );      break;
            }
            WinSetDlgItemText( hwnd, IDD_ENCODING, szEncoding );

            if ( codepage.attributes.subchar_len < 1 )
                sprintf( szSubChar, "(none)");
            else {
                sprintf( szSubChar, "0x");
                for ( i = 0; i < codepage.attributes.subchar_len; i++ ) {
                    sprintf( szBuf, "%02X", codepage.attributes.subchar[i] );
                    strcat( szSubChar, szBuf );
                }
            }
            WinSetDlgItemText( hwnd, IDD_SUBCHAR, szSubChar );

            for ( i = 0; i < 256; i++ ) {
                sprintf( szListItem, "0x%02X", i );
                switch ( codepage.fByteType[i] ) {
                    case ULS_SBCHAR:
                        WinSendDlgItemMsg( hwnd, IDD_SINGLE, LM_INSERTITEM, MPFROMSHORT( LIT_END ), MPFROMP(szListItem) );
                        break;
                    case ULS_2BYTE_LEAD:
                        WinSendDlgItemMsg( hwnd, IDD_2LEADING, LM_INSERTITEM, MPFROMSHORT( LIT_END ), MPFROMP(szListItem) );
                        break;
                    case ULS_3BYTE_LEAD:
                        WinSendDlgItemMsg( hwnd, IDD_3LEADING, LM_INSERTITEM, MPFROMSHORT( LIT_END ), MPFROMP(szListItem) );
                        break;
                    case ULS_4BYTE_LEAD:
                        WinSendDlgItemMsg( hwnd, IDD_4LEADING, LM_INSERTITEM, MPFROMSHORT( LIT_END ), MPFROMP(szListItem) );
                        break;
                    case ULS_CODEPOINT:
                        WinSendDlgItemMsg( hwnd, IDD_CODEPOINTS, LM_INSERTITEM, MPFROMSHORT( LIT_END ), MPFROMP(szListItem) );
                        break;
                }
            }
            for ( i = 0; i < 256; i++ ) {
                switch ( codepage.fFollowing[i] ) {
                    case 1:
                        sprintf( szListItem, "0x%02X", i );
                        WinSendDlgItemMsg( hwnd, IDD_FOLLOWING, LM_INSERTITEM, MPFROMSHORT( LIT_END ), MPFROMP(szListItem) );
                        break;
                    case 2:
                        sprintf( szListItem, "0x%02X (?)", i );
                        WinSendDlgItemMsg( hwnd, IDD_FOLLOWING, LM_INSERTITEM, MPFROMSHORT( LIT_END ), MPFROMP(szListItem) );
                        break;
                }
            }

            CentreWindow( hwnd );
            break;

        default: break;
    }

    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}


/* ------------------------------------------------------------------------- *
 * AboutDlgProc                                                              *
 *                                                                           *
 * Dialog procedure for the product information dialog.                      *
 * ------------------------------------------------------------------------- */
MRESULT EXPENTRY AboutDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    switch ( msg ) {

        case WM_INITDLG:
            CentreWindow( hwnd );
            break;

        default: break;
    }

    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}


/* ------------------------------------------------------------------------- *
 * QSCmpCP                                                                   *
 *                                                                           *
 * Comparison function for qsort() - compares two ULONG codepage numbers.    *
 * ------------------------------------------------------------------------- */
int QSCmpCP( const void *key, const void *element )
{
    if ( *((PULONG)key) < *((PULONG)element) ) return -1;
    else if ( *((PULONG)key) > *((PULONG)element) ) return 1;
    else return 0;
}


/* ------------------------------------------------------------------------- *
 * CentreWindow                                                              *
 *                                                                           *
 * Centres the given window on screen.                                       *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   HWND hwnd: Handle of the window to be centred.                          *
 *                                                                           *
 * RETURNS: N/A                                                              *
 * ------------------------------------------------------------------------- */
void CentreWindow( HWND hwnd )
{
    LONG scr_width, scr_height;
    LONG x, y;
    SWP wp;

    scr_width  = WinQuerySysValue( HWND_DESKTOP, SV_CXSCREEN );
    scr_height = WinQuerySysValue( HWND_DESKTOP, SV_CYSCREEN );

    if ( WinQueryWindowPos( hwnd, &wp )) {
        x = ( scr_width - wp.cx ) / 2;
        y = ( scr_height - wp.cy ) / 2;
        WinSetWindowPos( hwnd, HWND_TOP, x, y, wp.cx, wp.cy, SWP_MOVE | SWP_ACTIVATE );
    }

}


/* ------------------------------------------------------------------------- *
 * SelectFont                                                                *
 *                                                                           *
 * Allows the user to select the font used for displaying characters.        *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   HWND hwnd: Handle of the application (client) window.                   *
 *                                                                           *
 * RETURNS: N/A                                                              *
 * ------------------------------------------------------------------------- */
void SelectFont( HWND hwnd )
{
    FONTDLG     fontdlg;
    FONTMETRICS fm;
    HWND        hwndFD;
    LONG        lQuery = 0;
    CHAR        szName[ FACESIZE ],
                szTitle[ US_TITLE_MAX ];
    HPS         hps;


    hps = WinGetPS( hwnd );

    memset( &fontdlg, 0, sizeof(FONTDLG) );
    strncpy( szName, global.szFont, FACESIZE );

    // Get the metrics of the current font (we'll want to know the weight class)
    memset( &fm, 0, sizeof( fm ));
    lQuery = 1;
    GpiQueryFonts( hps, QF_PUBLIC, global.szFace, &lQuery, sizeof(fm), &fm );

    fontdlg.cbSize         = sizeof( FONTDLG );
    fontdlg.hpsScreen      = hps;
    fontdlg.pszTitle       = NULL;
    fontdlg.pszPreview     = NULL;
    fontdlg.pfnDlgProc     = NULL;
    fontdlg.pszFamilyname  = szName;
    fontdlg.usFamilyBufLen = sizeof( szName );
    fontdlg.fxPointSize    = MAKEFIXED( 10, 0 );
    fontdlg.usWeight       = (USHORT) fm.usWeightClass;
    fontdlg.clrFore        = SYSCLR_WINDOWTEXT;
    fontdlg.clrBack        = SYSCLR_WINDOW;
    fontdlg.fl             = FNTS_CENTER | FNTS_CUSTOM;
    fontdlg.flStyle        = 0;
    fontdlg.flType         = ( fm.fsSelection & FM_SEL_ITALIC ) ? FTYPE_ITALIC : 0;
    fontdlg.usDlgId        = IDD_FONTDLG;
    fontdlg.hMod           = NULLHANDLE;

    hwndFD = WinFontDlg( HWND_DESKTOP, hwnd, &fontdlg );
    // Repaint the window under the font dialog now, before the font is changed
    WinUpdateWindow( hwnd );

    if (( hwndFD ) && ( fontdlg.lReturn == DID_OK )) {
        strncpy( global.szFont, fontdlg.pszFamilyname,     FACESIZE-1 );
        strncpy( global.szFace, fontdlg.fAttrs.szFacename, FACESIZE-1 );

        // Find out if the new font can handle Unicode (from the metrics)
        memset( &fm, 0, sizeof( fm ));
        lQuery = 1;
        GpiQueryFonts( hps, QF_PUBLIC, fontdlg.fAttrs.szFacename, &lQuery, sizeof(fm), &fm );
        global.fUnicode = ( fm.fsType & FM_TYPE_UNICODE ) ? TRUE : FALSE;
        if ( !global.fUnicode && !CheckSupportedCodepage( hwnd, global.ulCP ))
            // (this will also update the titlebar)
            SetNewCodepage( hwnd, WinQueryCp( global.hmq ));
        else {
            // Update the titlebar with the new font
            if ( global.ulCP == 1 )
                sprintf( szTitle, "Viewing OS2UGL - %s", global.szFace );
            else
                sprintf( szTitle, "Viewing Codepage %u - %s", global.ulCP, global.szFace );
            WinSetWindowText( WinQueryWindow( hwnd, QW_PARENT ), szTitle );
        }
        WinInvalidateRegion( hwnd, NULLHANDLE, TRUE );
    }

    WinReleasePS( hps );
}


/* ------------------------------------------------------------------------- *
 * GetImageFont                                                              *
 *                                                                           *
 * Look for a bitmap (a.k.a. image or raster) font under the specified font  *
 * name; if possible, return the largest one which fits within the requested *
 * cell height.  However, if the requested height is too small for any of    *
 * the available bitmap fonts, then return the smallest one available -      *
 * unless the font includes an outline version as well, in which case just   *
 * return FALSE.  (A return value of FALSE implies that the caller should    *
 * treat the font name as an outline font.)                                  *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   HPS     hps        : Handle of the current presentation space.          *
 *   PSZ     pszFontFace: Face name of the font being requested.             *
 *   PFATTRS pfAttrs    : Address of a FATTRS object to receive the results. *
 *   LONG    lCY        : The requested cell height.                         *
 *                                                                           *
 * RETURNS: BOOL                                                             *
 *   TRUE if a suitable bitmap font was found; FALSE otherwise.              *
 * ------------------------------------------------------------------------- */
BOOL GetImageFont( HPS hps, PSZ pszFontFace, PFATTRS pfAttrs, LONG lCY )
{
    PFONTMETRICS pfm;               // array of FONTMETRICS objects
    LONG         i,                 // loop index
                 lSmIdx    = -1,    // index of smallest available font
                 lLargest  = 0,     // max baseline ext of largest font
                 cFonts    = 0,     // number of fonts found
                 cCount    = 0;     // number of fonts to return
    BOOL         fOutline  = FALSE, // does font include an outline version?
                 fFound    = FALSE; // did we find a suitable bitmap font?


    // Find the specific fonts which match the given face name
    cFonts = GpiQueryFonts( hps, QF_PUBLIC, pszFontFace,
                            &cCount, sizeof(FONTMETRICS), NULL );
    if ( cFonts < 1 ) return FALSE;
    if (( pfm = (PFONTMETRICS) calloc( cFonts, sizeof(FONTMETRICS) )) == NULL )
        return FALSE;
    GpiQueryFonts( hps, QF_PUBLIC, pszFontFace,
                   &cFonts, sizeof(FONTMETRICS), pfm );

    // Look for the largest bitmap font that fits within the requested height
    for ( i = 0; i < cFonts; i++ ) {
        if ( !( pfm[i].fsDefn & FM_DEFN_OUTLINE )) {
            if (( lSmIdx < 0 ) || ( pfm[i].lMaxBaselineExt < pfm[lSmIdx].lMaxBaselineExt ))
                lSmIdx = i;
            if (( pfm[i].lMaxBaselineExt <= lCY ) && ( pfm[i].lMaxBaselineExt > lLargest )) {
                lLargest = pfm[i].lMaxBaselineExt;
                fFound   = TRUE;
                strcpy( pfAttrs->szFacename, pfm[i].szFacename );
                pfAttrs->lMatch          = pfm[i].lMatch;
                pfAttrs->idRegistry      = pfm[i].idRegistry;
                pfAttrs->lMaxBaselineExt = pfm[i].lMaxBaselineExt;
                pfAttrs->lAveCharWidth   = pfm[i].lAveCharWidth;
            }
        } else fOutline = TRUE;
    }

    // If nothing fits within the requested height, use the smallest available
    if ( !fFound && !fOutline && lSmIdx >= 0 ) {
        fFound = TRUE;
        strcpy( pfAttrs->szFacename, pfm[lSmIdx].szFacename );
        pfAttrs->lMatch          = pfm[lSmIdx].lMatch;
        pfAttrs->idRegistry      = pfm[lSmIdx].idRegistry;
        pfAttrs->lMaxBaselineExt = pfm[lSmIdx].lMaxBaselineExt;
        pfAttrs->lAveCharWidth   = pfm[lSmIdx].lAveCharWidth;
    }

    free( pfm );
    return ( fFound );
}


/* ------------------------------------------------------------------------- *
 * SelectCodepage                                                            *
 *                                                                           *
 * Called when a user selects "Codepage" from the menu.  Prompts for a new   *
 * codepage, then switches to it.                                            *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   HWND hwnd: Handle of the application (client) window.                   *
 *                                                                           *
 * RETURNS: N/A                                                              *
 * ------------------------------------------------------------------------- */
void SelectCodepage( HWND hwnd )
{
    ULONG ulNewCP;

    ulNewCP = global.ulCP;
    WinDlgBox( HWND_DESKTOP, hwnd, (PFNWP) CodepageDlgProc, 0, IDD_CODEPAGE, &ulNewCP );
    if ( !ulNewCP || ulNewCP == global.ulCP ) return;

    if ( SetNewCodepage( hwnd, ulNewCP ))
        WinInvalidateRegion( hwnd, NULLHANDLE, TRUE );
}


/* ------------------------------------------------------------------------- *
 * SetNewCodepage                                                            *
 *                                                                           *
 * Performs the logic necessary when a new codepage gets selected.           *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   HWND hwnd:  Handle of the application (client) window.                  *
 *   ULONG ulCP: New codepage.                                               *
 *                                                                           *
 * RETURNS: BOOL                                                             *
 *   TRUE if the codepage was changed successfully.  May be FALSE if the     *
 *   conversion setup failed for some reason, or if the codepage is not      *
 *   supported by the current font/glyphlist mode.                           *
 * ------------------------------------------------------------------------- */
BOOL SetNewCodepage( HWND hwnd, ULONG ulCP )
{
    UconvObject uconvOld;
    CHAR        szTitle[ US_TITLE_MAX ];

    if ( !global.fUnicode && !CheckSupportedCodepage( hwnd, ulCP ))
        return FALSE;

    global.ulCP = ulCP;

    // Try to create a new conversion object for the selected codepage
    uconvOld = global.uconvCP;
    if ( SetupConversions( global.ulCP ) != ULS_SUCCESS ) {
        global.uconvCP = uconvOld;
        return FALSE;
    }
    if ( uconvOld != NULL ) UniFreeUconvObject( uconvOld );

    // Create the array of converted character values
    global.fCharMode = CH_MODE_SINGLE;
    memset( global.szOutput1, 0, sizeof(global.szOutput1) );
    GenerateOutput();
    if ( global.ulCP == 1 )
        sprintf( szTitle, "Viewing OS2UGL - %s", global.szFace );
    else
        sprintf( szTitle, "Viewing Codepage %u - %s", global.ulCP, global.szFace );
    WinSetWindowText( WinQueryWindow( hwnd, QW_PARENT ), szTitle );

    return TRUE;
}


/* ------------------------------------------------------------------------- *
 * CheckSupportedCodepage                                                    *
 *                                                                           *
 * See if the specified codepage is supported in non-Unicode output mode (a  *
 * function of what font is in use), and puts up an error message if not.    *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   HWND hwnd:  Handle of the application (client) window.                  *
 *   ULONG ulCP: Codepage to check.                                          *
 *                                                                           *
 * RETURNS: BOOL                                                             *
 * ------------------------------------------------------------------------- */
BOOL CheckSupportedCodepage( HWND hwnd, ULONG ulCP )
{
    ULONG  ulCount,
           i;
    PULONG aulCP;
    CHAR   szErr[ US_ERRTEXT_MAX + 1 ];
    BOOL   fOK;

    fOK = FALSE;
    if (( ulCP != 1200 ) && ( ulCP != 1207 ) && ( ulCP != 1208 )) {
        ulCount = WinQueryCpList( global.hab, 0, NULL );
        if (( aulCP = (PULONG) malloc( ulCount * sizeof( ULONG ))) != NULL ) {
            WinQueryCpList( global.hab, ulCount, aulCP );
            for ( i = 0; i < ulCount; i++ ) {
                if ( ulCP == aulCP[i] ) {
                    fOK = TRUE;
                    break;
                }
            }
            free( aulCP );
        }
    }
    if ( !fOK ) {
        sprintf( szErr, "Codepage %u is not supported by this font.", ulCP );
        ErrorPopup( szErr );
    }

    return ( fOK );
}


/* ------------------------------------------------------------------------- *
 * SetupConversions                                                          *
 *                                                                           *
 * Initialize the ULS conversion object for the selected codepage; also      *
 * populates the byte-type arrays for leading and following bytes.           *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   ULONG ulCP: The selected codepage number.                               *
 *                                                                           *
 * RETURNS: ULONG                                                            *
 *   0 or ULS error code.                                                    *
 * ------------------------------------------------------------------------- */
ULONG SetupConversions( ULONG ulCP )
{
    UniChar psuCP[ US_CPSPEC_MAX ];     // codepage conversion specifier
    CHAR    szError[ US_ERRTEXT_MAX ];  // error string
    ULONG   ulRC,
            i;
    BOOL    fHasFollowing;


    // Create a conversion object for the requested codepage
    memset( psuCP, 0, US_CPSPEC_MAX );
    if ( ulCP ) {
        if ( ulCP == 1 ) UniStrcat( psuCP, (UniChar *) L"OS2UGL");
        else {
            ulRC = UniMapCpToUcsCp( ulCP, psuCP, US_CPNAME_MAX );
            if ( ulRC != ULS_SUCCESS ) {
                sprintf( szError, "UniMapCpToUcsCp() error: 0x%X", ulRC );
                ErrorPopup( szError );
                return ( ulRC );
            }
        }
    }
    UniStrcat( psuCP, (UniChar *) L"@map=display,path=no");
    ulRC = UniCreateUconvObject( psuCP, &global.uconvCP );
    if ( ulRC != ULS_SUCCESS ) {
        if ( ulRC == ULS_INVALID )
            sprintf( szError, "%u is not recognized as a valid codepage.", ulCP );
        else
            sprintf( szError, "UniCreateUconvObject() error: 0x%X", ulRC );
        ErrorPopup( szError );
        return ( ulRC );
    }

    // Query available information about the codepage
    UniQueryUconvObject( global.uconvCP,
                         &(codepage.attributes), sizeof(codepage.attributes),
                         codepage.fByteType, codepage.fFollowing, codepage.user_range );

    /****
     * Now we have to work around some bugs in the reported information.
     ****/

    /*
     * Ken Borgendale's replacement IBM-949 seems to incorrectly report 0x5C as
     * an invalid codepoint.
     */
    if ( ulCP == 949 && codepage.fByteType[0x5C] == ULS_CODEPOINT )
        codepage.fByteType[0x5C] = ULS_SBCHAR;

    /* OS2UGL is a fixed-width 2-byte encoding but only has a few possible
     * lead values.
     */
    else if ( ulCP == 1 ) {
        codepage.fByteType[ 0 ] = ULS_2BYTE_LEAD;
        for ( i = 5; i < 256; i++ ) codepage.fByteType[ i ] = ULS_CODEPOINT;
        return ( ULS_SUCCESS );     // no need to continue
    }

    /*
     * fFollowing[] as returned from UniQueryUconvObject() isn't very reliable,
     * especially with 3- and 4-byte codepages.  So now we try to fix it up...
     */
    fHasFollowing = FALSE;
    // First, see if codepage reports any valid secondary bytes at all.
    for ( i = 0; i < 256; i++ ) {
        if ( codepage.fFollowing[i] ) {
            fHasFollowing = TRUE;
            break;
        }
    }
    /*
     * OK, so if the codepage allows multiple bytes per character, but no legal
     * secondary bytes are reported, it means we'll have to do some guesswork.
     * Note: we use 2 instead of 1 to indicate a "guessed" fFollowing[] flag
     *       (so that the codepage info dialog can display them differently).
     */
    if (( codepage.attributes.mb_max_len > 1 ) && ( !fHasFollowing )) {
        for ( i = 0; i < 256; i++ ) {

            // There are a few specific codepages we know how to deal with...
            switch ( ulCP ) {

                case 954:   // EUC-JP
                case 964:   // EUC-TW
                case 970:   // EUC-KR
                case 1383:  // EUC-CN
                    // Anything less than 0xA1 cannot be a secondary byte in EUC.
                    if ( i < 0xA1 )
                        codepage.fFollowing[ i ] = 0;
                    else
                        codepage.fFollowing[ i ] = 2;
                    break;

                case 1207:  // UPF-8 (internal PM Unicode encoding)

                    // 80- never occurs as a lead byte.
                    if ( i == 0x80 )
                        codepage.fByteType[ i ] = ULS_CODEPOINT;
                    // EC-EF are 3-byte lead values but aren't reported as such.
                    else if (( i >= 0xEC ) && ( i <= 0xEF ))
                        codepage.fByteType[ i ] = ULS_3BYTE_LEAD;
                    // Conversely, F0-F3 are unused, but misreported as 3-byte leads.
                    else if (( i >= 0xF0 ) && ( i <= 0xF3 ))
                        codepage.fByteType[ i ] = ULS_CODEPOINT;

                    // Anything less than 0x80 never occurs as a secondary byte.
                    if ( i < 0x80 )
                        codepage.fFollowing[ i ] = 0;
                    else
                        codepage.fFollowing[ i ] = 2;
                    break;

                case 1208:  // UTF-8
                    // C0-, C1-, or anything >= F5- are illegal UTF-8 values.
                    // And UCS-2 (& thus OS/2) doesn't even support >= F0-.
                    if (( i == 0xC0 ) || ( i == 0xC1 ) || ( i >= 0xF0 )) {
                        codepage.fByteType[ i ] = ULS_CODEPOINT;
                        break;
                    }
                    // A lead byte or 1-byte character cannot be a valid secondary byte.
                    if ( ISLEADINGBYTE( codepage.fByteType[i] ) || codepage.fByteType[i] == ULS_SBCHAR )
                        codepage.fFollowing[i] = 0;
                    else
                        codepage.fFollowing[i] = 2;
                    break;

                // Otherwise, just assume everything is a valid secondary byte.
                default:
                    codepage.fFollowing[ i ] = 2;
                    break;
            }
        }
    }

    return ( ULS_SUCCESS );
}


/* ------------------------------------------------------------------------- *
 * GenerateOutput                                                            *
 *                                                                           *
 * Generate the output for the topmost level (single-byte characters and     *
 * leading-byte values).  This is done when the codepage is first requested. *
 * (Calculation of secondary codepoints is deferred until a specific leading *
 * byte is selected.)                                                        *
 *                                                                           *
 * ARGUMENTS: none                                                           *
 *                                                                           *
 * RETURNS: N/A                                                              *
 * ------------------------------------------------------------------------- */
ULONG GenerateOutput( void )
{
    UCHAR   szVal[ 2 ];            // the current character as a codepage string
    UniChar suVal[ UCS_OUT_LEN ],  // the current character as a UCS-2 string
            *psu;                  // UCS string pointer used by UniUconv*Ucs()
    PSZ     psz;                   // string pointer used by UniUconv*Ucs()
    size_t  stIn, stOut, stSub;    // size values used by UniUconv*Ucs()
    USHORT  ucVal;                 // the current character's byte value
    ULONG   ulRC;


    for ( ucVal = 0; ucVal < 256; ucVal++ ) {
        memset( szVal, 0, sizeof(szVal) );
        memset( suVal, 0, sizeof(suVal) );

        codepage.flValues1[ ucVal ] = 0;

        // For each codepoint, generate the appropriate Unicode value.
        switch ( codepage.fByteType[ ucVal ] ) {

            case ULS_SBCHAR:        // Single-byte character: convert to UCS-2 normally
                szVal[ 0 ] = ucVal;
#if 1
                stIn  = 1;
                stOut = UCS_OUT_LEN - 1;
                stSub = 0;
                psz   = szVal;
                psu   = suVal;
                ulRC  = UniUconvToUcs( global.uconvCP, (PPVOID)&psz, &stIn, &psu, &stOut, &stSub );
#else
                ulRC = UniStrToUcs( global.uconvCP, suVal, szVal, 2 );
#endif
                suVal[ 1 ] = 0x0000;
                break;

            case ULS_2BYTE_LEAD:    // Multi-byte character: represent using U+303F
                codepage.flValues1[ ucVal ] |= CPV_1_MORE;
                suVal[ 0 ] = 0x303F;
                ulRC = ULS_SUCCESS;
                break;
            case ULS_3BYTE_LEAD:
                codepage.flValues1[ ucVal ] |= CPV_2_MORE;
                suVal[ 0 ] = 0x303F;
                ulRC = ULS_SUCCESS;
                break;
            case ULS_4BYTE_LEAD:
                codepage.flValues1[ ucVal ] |= CPV_3_MORE;
                suVal[ 0 ] = 0x303F;
                ulRC = ULS_SUCCESS;
                break;

            case ULS_CODEPOINT:     // Not a valid character: leave as NULL (U+0000)
                codepage.flValues1[ ucVal ] |= CPV_CODEPOINT;
                ulRC = ULS_SUCCESS;
                break;

            default:                // Unknown codepoint type: represent using U+2318
                suVal[ 0 ] = 0x2318;
                ulRC = ULS_SUCCESS;

        }

        if ( ulRC != ULS_SUCCESS ) {
            if ( ! ISLEADINGBYTE( codepage.fByteType[ucVal] ))
                codepage.flValues1[ ucVal ] |= CPV_UCS_FAILED;
            if ( global.fUCS )
                sprintf( global.szOutput1[ucVal], "????");
            else
                sprintf( global.szOutput1[ucVal], "");
        }

        else {
            if ( global.fUCS ) {                 // Output Unicode hex value
                if ( ISLEADINGBYTE( codepage.fByteType[ucVal] ))
                    sprintf( global.szOutput1[ucVal], "****");
                else
                    sprintf( global.szOutput1[ucVal], "%04X", suVal[0] );
            } else {                            // Output character glyph
                if ( codepage.fByteType[ucVal] == ULS_CODEPOINT && suVal[0] == 0xFFFD )
                    suVal[0] = 0;
#if 1
                stIn  = 1;
                stOut = US_SZOUT_MAX - 1;
                stSub = 0;
                psu   = suVal;
                psz   = global.szOutput1[ ucVal ];
                memset( psz, 0, US_SZOUT_MAX );
                ulRC = UniUconvFromUcs( global.uconvUPF, &psu, &stIn, (PPVOID)&psz, &stOut, &stSub );
#else
                ulRC = UniStrFromUcs( global.uconvUPF, global.szOutput1[ucVal], suVal, US_SZOUT_MAX );
#endif
                if ( ulRC != ULS_SUCCESS ) {
                    if ( ! ISLEADINGBYTE( codepage.fByteType[ucVal] ))
                        codepage.flValues1[ ucVal ] |= CPV_UPF_FAILED;
                    sprintf( global.szOutput1[ucVal], "");
                }
            }
        }
    }

    return ( ULS_SUCCESS );
}


/* ------------------------------------------------------------------------- *
 * GenerateOutputLvl2                                                        *
 *                                                                           *
 * Generate the output for byte values within a two-byte range (double-byte  *
 * characters, or the second bytes of three- & four- byte characters).  Also *
 * known as a level 2 ward.                                                  *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   UCHAR ucLeadByte: Current first byte value.                             *
 *                                                                           *
 * RETURNS: ULONG                                                            *
 *   0 (ULS_SUCCESS).                                                        *
 * ------------------------------------------------------------------------- */
ULONG GenerateOutputLvl2( UCHAR ucLeadByte )
{
    UCHAR   szVal[ 3 ];            // the current character as a codepage string
    PSZ     psz;                   // string pointer used by UniUconv*Ucs()
    UniChar suVal[ UCS_OUT_LEN ],  // the current character as a UCS-2 string
            *psu;                  // UCS string pointer used by UniUconv*Ucs()
    USHORT  ucVal;                 // the current character's second-byte value
    ULONG   flags,
            ulRC;
    size_t  stIn, stOut, stSub;    // size values used by UniUconv*Ucs()


    for ( ucVal = 0; ucVal < 256; ucVal++ ) {
        memset( szVal, 0, sizeof(szVal) );
        memset( suVal, 0, sizeof(suVal) );
        flags = 0;

        if ( global.ulCP == 1200 ) {
            // If we're already in UCS, we can just copy the value directly
            suVal[ 0 ] = UNICFROM2BYTES( ucLeadByte, ucVal );
        } else if ( ! codepage.fFollowing[ucVal] ) {
            // If this isn't a valid secondary byte, leave as NULL
            flags |= CPV_CODEPOINT;
        } else if (( global.ulCP == 1208 ) &&
                  ( ucLeadByte == 0xE0 ) && ( ucVal < 0xA0 )) {
            // UTF-8 doesn't use these codepoints
            flags |= CPV_CODEPOINT;
        } else if ( codepage.fByteType[ucLeadByte] == ULS_3BYTE_LEAD ) {
            flags |= CPV_1_MORE;
            suVal[ 0 ] = 0x303F;
        } else if ( codepage.fByteType[ucLeadByte] == ULS_4BYTE_LEAD ) {
            flags |= CPV_2_MORE;
            suVal[ 0 ] = 0x303F;
        } else {
            // Convert the character
            if ( global.ulCP == 1 ) {
                // (OS2UGL works with numbers rather than characters, so we
                // have to swap the bytes around into endian order)
                szVal[ 0 ] = ucVal;
                szVal[ 1 ] = ucLeadByte;
            } else {
                szVal[ 0 ] = ucLeadByte;
                szVal[ 1 ] = ucVal;
            }
            stIn  = 2;
            psz   = szVal;
            psu   = suVal;
            stOut = 1;
            stSub = 0;
            ulRC  = UniUconvToUcs( global.uconvCP, (PPVOID)&psz, &stIn, &psu, &stOut, &stSub );
            if ( ulRC != ULS_SUCCESS ) {
                flags |= CPV_UCS_FAILED;
                if ( global.fUCS ) sprintf( global.szOutput2[ ucVal ], "????");
                else sprintf( global.szOutput2[ ucVal ], "");
            }
        }
        suVal[ 1 ] = 0x0000;

        if ( global.fUCS ) {                        // Output Unicode hex value
            if (( flags & CPV_1_MORE ) || ( flags & CPV_2_MORE ))
                sprintf( global.szOutput2[ ucVal ], "****");
            else
                sprintf( global.szOutput2[ ucVal ], "%04X", suVal[0] );
        } else {                                    // Output character glyph
            if (( suVal[0] >= 0xD800 ) && ( suVal[0] <= 0xDFFF )) {
                memset( suVal, 0, sizeof(suVal) );
                flags |= CPV_SURROGATE;
            }
            psu   = suVal;
            psz   = global.szOutput2[ ucVal ];
            stIn  = 1;
            stOut = US_SZOUT_MAX - 1;
            stSub = 0;
            memset( psz, 0, US_SZOUT_MAX );
            ulRC = UniUconvFromUcs( global.uconvUPF, &psu, &stIn, (PPVOID)&psz, &stOut, &stSub );
            if ( ulRC != ULS_SUCCESS ) {
                flags |= CPV_UPF_FAILED;
                sprintf( global.szOutput2[ ucVal ], "");
            }
        }
        codepage.flValues2[ ucVal ] = flags;
    }

    return ( ULS_SUCCESS );
}


/* ------------------------------------------------------------------------- *
 * GenerateOutputLvl3                                                        *
 *                                                                           *
 * Generate the output for byte values within a three-byte range (three-byte *
 * characters, or the third bytes of four-byte characters).  Also known as a *
 * level 3 ward.                                                             *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   UCHAR ucFirst : Current first byte value.                               *
 *   UCHAR ucSecond: Current second byte value.                              *
 *                                                                           *
 * RETURNS: ULONG                                                            *
 *   0 (ULS_SUCCESS).                                                        *
 * ------------------------------------------------------------------------- */
ULONG GenerateOutputLvl3( UCHAR ucFirst, UCHAR ucSecond )
{
    UCHAR   szVal[ 4 ];            // the current character as a codepage string
    PSZ     psz;                   // string pointer used by UniUconv*Ucs()
    UniChar suVal[ UCS_OUT_LEN ],  // the current character as a UCS-2 string
            *psu;                  // UCS string pointer used by UniUconv*Ucs()
    USHORT  ucVal;                 // the current character's third-byte value
    ULONG   flags,                 // special flags for the current codepoint
            ulRC;
    size_t  stIn, stOut, stSub;    // size values used by UniUconv*Ucs()


    for ( ucVal = 0; ucVal < 256; ucVal++ ) {
        memset( szVal, 0, sizeof(szVal) );
        memset( suVal, 0, sizeof(suVal) );
        flags = 0;

        if ( ! codepage.fFollowing[ucVal] ) {
            // If this isn't a valid secondary byte, leave as NULL
            flags |= CPV_CODEPOINT;
        } else if ( codepage.fByteType[ucFirst] == ULS_4BYTE_LEAD ) {
            // Third byte of a four-byte character
            flags |= CPV_1_MORE;
            suVal[ 0 ] = 0x303F;
        } else {
            // Convert the character
            szVal[ 0 ] = ucFirst;
            szVal[ 1 ] = ucSecond;
            szVal[ 2 ] = ucVal;
#if 1
            stIn  = 3;
            psz   = szVal;
            psu   = suVal;
            stOut = 1;
            stSub = 0;
            ulRC  = UniUconvToUcs( global.uconvCP, (PPVOID)&psz, &stIn, &psu, &stOut, &stSub );
#else
            ulRC = UniStrToUcs( global.uconvCP, suVal, szVal, sizeof(suVal) );
#endif
            if ( ulRC != ULS_SUCCESS ) {
                flags |= CPV_UCS_FAILED;
                if ( global.fUCS ) sprintf( global.szOutput3[ ucVal ], "????");
                else sprintf( global.szOutput3[ ucVal ], "");
            }
        }
        suVal[ 1 ] = 0x0000;

        if ( global.fUCS ) {                        // Output Unicode hex value
            if ( flags & CPV_1_MORE )
                sprintf( global.szOutput3[ ucVal ], "****");
            else
                sprintf( global.szOutput3[ ucVal ], "%04X", suVal[0] );
        } else {                                    // Output character glyph
            if (( suVal[0] >= 0xD800 ) && ( suVal[0] <= 0xDFFF )) {
                memset( suVal, 0, sizeof(suVal) );
                flags |= CPV_SURROGATE;
            }
#if 1
            psu   = suVal;
            psz   = global.szOutput3[ ucVal ];
            stIn  = 1;
            stOut = US_SZOUT_MAX - 1;
            stSub = 0;
            memset( psz, 0, US_SZOUT_MAX );
            ulRC = UniUconvFromUcs( global.uconvUPF, &psu, &stIn, (PPVOID)&psz, &stOut, &stSub );
#else
            ulRC = UniStrFromUcs( global.uconvUPF, global.szOutput3[ ucVal ], suVal, US_SZOUT_MAX );
#endif
            if ( ulRC != ULS_SUCCESS ) {
                flags |= CPV_UPF_FAILED;
                sprintf( global.szOutput3[ ucVal ], "");
            }
        }

        codepage.flValues3[ ucVal ] = flags;
    }

    return ( ULS_SUCCESS );
}


/* ------------------------------------------------------------------------- *
 * GenerateOutputLvl4                                                        *
 *                                                                           *
 * Generate the output for byte values within a four-byte range (four-byte   *
 * characters, which are the maximum possible).  Also known as a level 4     *
 * ward.                                                                     *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   UCHAR ucFirst : Current first byte value.                               *
 *   UCHAR ucSecond: Current second byte value.                              *
 *   UCHAR ucThird : Current third byte value.                               *
 *                                                                           *
 * RETURNS: ULONG                                                            *
 *   0 (ULS_SUCCESS).                                                        *
 * ------------------------------------------------------------------------- */
ULONG GenerateOutputLvl4( UCHAR ucFirst, UCHAR ucSecond, UCHAR ucThird )
{
    UCHAR   szVal[ 5 ];            // the current character as a codepage string
    PSZ     psz;                   // string pointer used by UniUconv*Ucs()
    UniChar suVal[ UCS_OUT_LEN ],  // the current character as a UCS-2 string
            *psu;                  // UCS string pointer used by UniUconv*Ucs()
    USHORT  ucVal;                 // the current character's fourth-byte value
    ULONG   flags,                 // special flags for the current codepoint
            ulRC;
    size_t  stIn, stOut, stSub;    // size values used by UniUconv*Ucs()


    for ( ucVal = 0; ucVal < 256; ucVal++ ) {
        memset( szVal, 0, sizeof(szVal) );
        memset( suVal, 0, sizeof(suVal) );
        flags = 0;

        if ( ! codepage.fFollowing[ucVal] ) {
            // If this isn't a valid secondary byte, leave as NULL
            flags |= CPV_CODEPOINT;
        } else {
            // Convert the character
            szVal[ 0 ] = ucFirst;
            szVal[ 1 ] = ucSecond;
            szVal[ 2 ] = ucThird;
            szVal[ 3 ] = ucVal;
#if 1
            stIn  = 4;
            psz   = szVal;
            psu   = suVal;
            stOut = 1;
            stSub = 0;
            ulRC  = UniUconvToUcs( global.uconvCP, (PPVOID)&psz, &stIn, &psu, &stOut, &stSub );
#else
            ulRC = UniStrToUcs( global.uconvCP, suVal, szVal, sizeof(suVal) );
#endif
            if ( ulRC != ULS_SUCCESS ) {
                flags |= CPV_UCS_FAILED;
                if ( global.fUCS ) sprintf( global.szOutput4[ ucVal ], "????");
                else sprintf( global.szOutput4[ ucVal ], "");
            }
        }
        suVal[ 1 ] = 0x0000;

        if ( global.fUCS ) {                        // Output Unicode hex value
            sprintf( global.szOutput4[ ucVal ], "%04X", suVal[0] );
        } else {                                    // Output character glyph
#if 1
            psu   = suVal;
            psz   = global.szOutput4[ ucVal ];
            stIn  = 1;
            stOut = US_SZOUT_MAX - 1;
            stSub = 0;
            memset( psz, 0, US_SZOUT_MAX );
            ulRC = UniUconvFromUcs( global.uconvUPF, &psu, &stIn, (PPVOID)&psz, &stOut, &stSub );
#else
            ulRC = UniStrFromUcs( global.uconvUPF, global.szOutput4[ ucVal ], suVal, US_SZOUT_MAX );
#endif
            if ( ulRC != ULS_SUCCESS ) {
                flags |= CPV_UPF_FAILED;
                sprintf( global.szOutput4[ ucVal ], "");
            }
        }

        codepage.flValues4[ ucVal ] = flags;
    }

    return ( ULS_SUCCESS );
}


/* ------------------------------------------------------------------------- *
 * PaintClient                                                               *
 *                                                                           *
 * Handles (re)painting of the client window; called by WM_PAINT.            *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   HWND hwnd: Handle of the application (client) window.                   *
 *                                                                           *
 * RETURNS: (MRESULT) 0                                                      *
 * ------------------------------------------------------------------------- */
MRESULT PaintClient( HWND hwnd )
{
    HPS         hps;            // local presentation space handle
    RECTL       rcl,            // various GPI data we use for calculations
                rclClip;        // ...
    POINTL      ptl;            // ...
    FONTMETRICS fontMetrics;    // ...
    FATTRS      fontAttrs;      // ...
    SIZEF       sfChSize;       // ...
    LONG        lRc,            // ...
                xCurrent,       // current X coordinate (left of current cell)
                yCurrent,       // current Y coordinate (bottom of current cell)
                yTextOffset,    // offset of the baseline within the current cell
                alClrs[ 5 ];    // array of RGB values to add to the color table
    UCHAR       row, col,       // row, column in 16x16 byte value grid
                szByteH[ 5 ];   // byte row/column heading
    int         csz;            // character box size we will use for output


    hps = WinBeginPaint( hwnd, NULLHANDLE, NULLHANDLE );

    // Define some new colours
    alClrs[0] = 0x00E0E0E0;     // very pale grey
    alClrs[1] = 0x00FFE0E0;     // very pale red
    alClrs[2] = 0x00E0FFE0;     // very pale green
    alClrs[3] = 0x00E0E0FF;     // very pale blue
    alClrs[4] = 0x00FFFFE0;     // cream
    GpiCreateLogColorTable( hps, 0, LCOLF_CONSECRGB, 16, 5, alClrs );

    WinQueryWindowRect( hwnd, &rcl );
    WinFillRect( hps, &rcl, global.fUnicode? SYSCLR_WINDOW: 20 );

    // Calculate some basic measurements
    coord.xMargin     = 2;
    coord.yMargin     = 2;
    coord.xCellWidth  = (rcl.xRight - (coord.xMargin * 2) - 1) / 17;
    coord.yCellHeight = (rcl.yTop - (coord.yMargin * 2) - 1) / 17;
    coord.xHeadWidth  = coord.xCellWidth;
    coord.yHeadHeight = coord.yCellHeight;

    // Determine the appropriate font size
    csz = ((coord.xCellWidth < coord.yCellHeight) ? coord.xCellWidth : coord.yCellHeight ) / 2;
    if ( global.fUCS ) csz -= 2;
    else               csz += 4;

    // Set the character font
    memset( &fontAttrs, 0, sizeof( FATTRS ));
    fontAttrs.usRecordLength = sizeof( FATTRS );
    fontAttrs.usCodePage     = global.fUnicode ? 1207 : global.ulCP;
    fontAttrs.fsType         = FATTR_TYPE_MBCS;
    fontAttrs.fsFontUse      = FATTR_FONTUSE_NOMIX;
    global.fRaster = GetImageFont( hps, global.szFace, &fontAttrs, csz );
    if ( ! global.fRaster )
        strcpy( fontAttrs.szFacename, global.szFace );

    lRc = GpiCreateLogFont( hps, NULL, 1L, &(fontAttrs) );
    if ( lRc == GPI_ERROR ) ErrorPopup("Could not create logical font.");
    else GpiSetCharSet( hps, 1L );

    // Set the character cell size
    sfChSize.cx = MAKEFIXED( csz, 0 );
    sfChSize.cy = sfChSize.cx;
    GpiSetCharBox( hps, &sfChSize );
    GpiSetTextAlignment( hps, TA_CENTER, TA_BASE );

    // Get the font metrics & calculate our remaining measurements
    GpiQueryFontMetrics( hps, sizeof( FONTMETRICS ), &fontMetrics );
    yTextOffset = ( coord.yCellHeight - fontMetrics.lXHeight ) / 2;
    if ( yTextOffset < fontMetrics.lMaxDescender )
        yTextOffset = fontMetrics.lMaxDescender;

    // Draw the grid
    GpiSetColor( hps, CLR_DARKGRAY );
    // (horizontal lines)
    yCurrent = rcl.yTop - coord.yMargin - coord.yHeadHeight;
    xCurrent = coord.xMargin + coord.xHeadWidth;
    for ( row = 0; row < 17; row++ ) {
        ptl.x = xCurrent;
        ptl.y = yCurrent;
        GpiMove( hps, &ptl );
        ptl.x += coord.xCellWidth * 16;
        GpiLine( hps, &ptl );
        yCurrent -= coord.yCellHeight;
    }
    // (vertical lines)
    yCurrent = rcl.yTop - coord.yMargin - coord.yHeadHeight;
    for ( col = 0; col < 17; col++ ) {
        ptl.x = xCurrent;
        ptl.y = yCurrent;
        GpiMove( hps, &ptl );
        ptl.y -= coord.yCellHeight * 16;
        GpiLine( hps, &ptl );
        xCurrent += coord.xCellWidth;
    }
    // (column headings)
    xCurrent = coord.xMargin + coord.xHeadWidth;
    yCurrent = rcl.yTop - coord.yMargin - coord.yHeadHeight;
    for ( col = 0; col < 16; col++ ) {
        sprintf( szByteH, "-%X", col );
        ptl.x = xCurrent + ( coord.xCellWidth / 2 );
        ptl.y = yCurrent + yTextOffset;
        rclClip.xLeft   = xCurrent + 1;
        rclClip.yBottom = yCurrent + 1;
        rclClip.xRight  = xCurrent + coord.xCellWidth  - 1;
        rclClip.yTop    = yCurrent + coord.yCellHeight - 1;
        GpiCharStringPosAt( hps, &ptl, &rclClip, CHS_CLIP, strlen(szByteH), szByteH, NULL );
        xCurrent += coord.xCellWidth;
    }
    // (row headings)
    xCurrent = coord.xMargin;
    for ( row = 0; row < 16; row++ ) {
        yCurrent -= coord.yCellHeight;
        sprintf( szByteH, "%X-", row );
        ptl.x = xCurrent + ( coord.xCellWidth / 2 );
        ptl.y = yCurrent + yTextOffset;
        rclClip.xLeft   = xCurrent + 1;
        rclClip.yBottom = yCurrent + 1;
        rclClip.xRight  = xCurrent + coord.xCellWidth  - 1;
        rclClip.yTop    = yCurrent + coord.yCellHeight - 1;
        GpiCharStringPosAt( hps, &ptl, &rclClip, CHS_CLIP, strlen(szByteH), szByteH, NULL );
    }

    // Now print the character for each byte value in the grid
    switch( global.fCharMode ) {
        case CH_MODE_DOUBLE:    // print double-byte characters
            PaintChars2B( hps, rcl, yTextOffset, global.fUnicode );
            break;
        case CH_MODE_TRIPLE:    // print three-byte characters
            PaintChars3B( hps, rcl, yTextOffset, global.fUnicode );
            break;
        case CH_MODE_QUADRPL:   // print four-byte characters
            PaintChars4B( hps, rcl, yTextOffset, global.fUnicode );
            break;
        case CH_MODE_SINGLE:    // print single-byte characters
        default:
            PaintChars1B( hps, rcl, yTextOffset, global.fUnicode );
            break;
    }

    WinEndPaint( hps );
    return ( 0 );
}


/* ------------------------------------------------------------------------- *
 * PaintChars1B                                                              *
 *                                                                           *
 * Populate the character cells within the top-level (single-byte) ward.     *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   HPS   hps        : Handle to the client window's presentation space.    *
 *   RECTL rcl        : The current boundaries of the client window.         *
 *   LONG  yTextOffset: The current vertical text offset within a cell.      *
 *                                                                           *
 * RETURNS: N/A                                                              *
 * ------------------------------------------------------------------------- */
void PaintChars1B( HPS hps, RECTL rcl, LONG yTextOffset, BOOL fUnicode )
{
    RECTL  rclCell;        // clipping rectangle for the current cell
    POINTL ptl;            // used for GPI positioning
    LONG   xCurrent,       // current X coordinate (left of current cell)
           yCurrent;       // current Y coordinate (bottom of current cell)
    UCHAR  row, col,       // row, column in 16x16 byte value grid
           ucVal;          // the current character's byte value
    CHAR   szCpVal[ 2 ];   // current character as a native codepage string
    PSZ    pszOutput;      // pointer to rendered character string

    yCurrent = rcl.yTop - coord.yMargin - coord.yHeadHeight;
    for ( row = 0; row < 16; row++ ) {
        xCurrent = coord.xMargin + coord.xHeadWidth;
        yCurrent -= coord.yCellHeight;
        for ( col = 0; col < 16; col++ ) {
            ucVal = ( 16 * row ) + col;
            szCpVal[ 0 ] = ucVal;
            szCpVal[ 1 ] = '\0';

            /* Check for special byte types.  (These should all be available by
             * checking flValues1[] as well, but for leading bytes the absolute
             * values in fByteType[] allow us to use switch(), which is easier.)
             */
            switch ( codepage.fByteType[ucVal] ) {
                case ULS_SBCHAR:      GpiSetColor( hps, CLR_DEFAULT );   break;
                case ULS_2BYTE_LEAD:  GpiSetColor( hps, CLR_DARKBLUE );  break;
                case ULS_3BYTE_LEAD:  GpiSetColor( hps, CLR_DARKGREEN ); break;
                case ULS_4BYTE_LEAD:  GpiSetColor( hps, CLR_DARKCYAN );  break;
                case ULS_CODEPOINT:   GpiSetColor( hps, CLR_PALEGRAY );  break;
                default:              GpiSetColor( hps, CLR_BROWN );     break;
            }
            // Check for a couple of other conditions not covered by fByteType:
            if ( codepage.flValues1[ ucVal ] & CPV_UCS_FAILED )
                GpiSetColor( hps, CLR_RED );
            else if ( codepage.flValues1[ ucVal ] & CPV_UPF_FAILED )
                GpiSetColor( hps, CLR_DARKRED );

            // Get the current cell rectangle
            rclCell.xLeft   = xCurrent + 1;
            rclCell.yBottom = yCurrent + 1;
            rclCell.xRight  = xCurrent + coord.xCellWidth - 1;
            rclCell.yTop    = yCurrent + coord.yCellHeight - 1;

            // Colour in the cell for special byte values
            if (( codepage.fByteType[ucVal] > 1 ) || ( codepage.flValues1[ ucVal ] > 0 )) {
                ptl.x = rclCell.xLeft;
                ptl.y = rclCell.yBottom;
                GpiMove( hps, &ptl );
                ptl.x += coord.xCellWidth - 2;
                ptl.y += coord.yCellHeight - 2;
                GpiBox( hps, DRO_FILL, &ptl, 0, 0 );
                GpiSetColor( hps, CLR_PALEGRAY );
            }
            ptl.x = xCurrent + ( coord.xCellWidth  / 2 );
            ptl.y = yCurrent + yTextOffset; //( coord.yCellHeight / 2 );
            pszOutput = (fUnicode | global.fUCS) ? global.szOutput1[ ucVal ] : szCpVal;
            GpiCharStringPosAt( hps, &ptl, &rclCell, CHS_CLIP,
                                strlen(pszOutput), pszOutput, NULL );

            xCurrent += coord.xCellWidth;
        }
    }

}


/* ------------------------------------------------------------------------- *
 * PaintChars2B                                                              *
 *                                                                           *
 * Populate the character cells within a level 2 (2-byte) ward.              *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   HPS   hps        : Handle to the client window's presentation space.    *
 *   RECTL rcl        : The current boundaries of the client window.         *
 *   LONG  yTextOffset: The current vertical text offset within a cell.      *
 *                                                                           *
 * RETURNS: N/A                                                              *
 * ------------------------------------------------------------------------- */
void PaintChars2B( HPS hps, RECTL rcl, LONG yTextOffset, BOOL fUnicode )
{
    RECTL  rclCell;        // clipping rectangle for the current cell
    POINTL ptl;            // used for GPI positioning
    LONG   xCurrent,       // current X coordinate (left of current cell)
           yCurrent;       // current Y coordinate (bottom of current cell)
    UCHAR  row, col,       // row, column in 16x16 byte value grid
           ucVal;          // the current character's byte value
    CHAR   szCpVal[ 3 ];   // current character as a native codepage string
    PSZ    pszOutput;      // pointer to rendered character string
    BOOL   fFill;          // indicates whether cell should be shaded
    ULONG  flags;


    yCurrent = rcl.yTop - coord.yMargin - coord.yHeadHeight;
    for ( row = 0; row < 16; row++ ) {
        xCurrent = coord.xMargin + coord.xHeadWidth;
        yCurrent -= coord.yCellHeight;
        for ( col = 0; col < 16; col++ ) {
            ucVal = ( 16 * row ) + col;
            szCpVal[ 0 ] = global.ucFirst;
            szCpVal[ 1 ] = ucVal;
            szCpVal[ 2 ] = '\0';

            /* See if we need to treat this codepoint specially (by colouring
             * the cell somehow) according to whether or not it was flagged
             * during conversion.  Unlike in PaintChars1B(), we can't simply
             * check the fByteType[] array, since that only covers single-byte
             * codepoints.  So we rely on the flValues2[] flags.
             */
            flags = codepage.flValues2[ ucVal ];
            if ( !flags ) {                            // displayable character
                GpiSetColor( hps, CLR_DEFAULT );
                fFill = FALSE;
            } else {
                fFill = TRUE;
                if ( flags & CPV_CODEPOINT )           // illegal value
                    GpiSetColor( hps, CLR_PALEGRAY );
                else if ( flags & CPV_SURROGATE )      // high or low surrogate
                    GpiSetColor( hps, CLR_DARKPINK );
                else if ( flags & CPV_1_MORE )         // probable 3-byte codepoint
                    GpiSetColor( hps, CLR_DARKBLUE );
                else if ( flags & CPV_2_MORE )         // probable 4-byte codepoint
                    GpiSetColor( hps, CLR_DARKGREEN );
                else if ( flags & CPV_UCS_FAILED )     // conversion to UCS failed
                    GpiSetColor( hps, CLR_RED );
                else if ( flags & CPV_UPF_FAILED )     // conversion from UCS failed
                    GpiSetColor( hps, CLR_DARKRED );
            }

            // Get the current cell rectangle
            rclCell.xLeft   = xCurrent + 1;
            rclCell.yBottom = yCurrent + 1;
            rclCell.xRight  = xCurrent + coord.xCellWidth - 1;
            rclCell.yTop    = yCurrent + coord.yCellHeight - 1;

            if ( fFill ) {
                ptl.x = rclCell.xLeft;
                ptl.y = rclCell.yBottom;
                GpiMove( hps, &ptl );
                ptl.x += coord.xCellWidth - 2;
                ptl.y += coord.yCellHeight - 2;
                GpiBox( hps, DRO_FILL, &ptl, 0, 0 );
                GpiSetColor( hps, CLR_PALEGRAY );
            }
            ptl.x = xCurrent + ( coord.xCellWidth  / 2 );
            ptl.y = yCurrent + yTextOffset;
            pszOutput = (fUnicode || global.fUCS) ? global.szOutput2[ ucVal ] : szCpVal;
//          memset( aptl, 0, sizeof(aptl) );
//          GpiQueryCharStringPosAt( hps, &ptl, 0, strlen(pszOutput), pszOutput, NULL, aptl );
            GpiCharStringPosAt( hps, &ptl, &rclCell, CHS_CLIP,
                                strlen(pszOutput), pszOutput, NULL );
            xCurrent += coord.xCellWidth;
        }
    }
}


/* ------------------------------------------------------------------------- *
 * PaintChars3B                                                              *
 *                                                                           *
 * Populate the character cells within a level 3 (3-byte) ward.              *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   HPS   hps        : Handle to the client window's presentation space.    *
 *   RECTL rcl        : The current boundaries of the client window.         *
 *   LONG  yTextOffset: The current vertical text offset within a cell.      *
 *                                                                           *
 * RETURNS: N/A                                                              *
 * ------------------------------------------------------------------------- */
void PaintChars3B( HPS hps, RECTL rcl, LONG yTextOffset, BOOL fUnicode )
{
    RECTL  rclCell;        // clipping rectangle for the current cell
    POINTL ptl;            // used for GPI positioning
    LONG   xCurrent,       // current X coordinate (left of current cell)
           yCurrent;       // current Y coordinate (bottom of current cell)
    UCHAR  row, col,       // row, column in 16x16 byte value grid
           ucVal;          // the current character's byte value
    CHAR   szCpVal[ 4 ];   // current character as a native codepage string
    PSZ    pszOutput;      // pointer to rendered character string
    BOOL   fFill;          // indicates whether cell should be shaded
    ULONG  flags;

    yCurrent = rcl.yTop - coord.yMargin - coord.yHeadHeight;
    for ( row = 0; row < 16; row++ ) {
        xCurrent = coord.xMargin + coord.xHeadWidth;
        yCurrent -= coord.yCellHeight;
        for ( col = 0; col < 16; col++ ) {
            ucVal = ( 16 * row ) + col;
            szCpVal[ 0 ] = global.ucFirst;
            szCpVal[ 1 ] = global.ucSecond;
            szCpVal[ 2 ] = ucVal;
            szCpVal[ 3 ] = '\0';

            // See if we need to colour in the cell.
            flags = codepage.flValues3[ ucVal ];
            if ( !flags ) {                         // displayable character
                GpiSetColor( hps, CLR_DEFAULT );
                fFill = FALSE;
            } else {
                fFill = TRUE;
                if ( flags & CPV_CODEPOINT )        // illegal value
                    GpiSetColor( hps, CLR_PALEGRAY );
                else if ( flags & CPV_SURROGATE )   // high or low surrogate
                    GpiSetColor( hps, CLR_DARKPINK );
                else if ( flags & CPV_1_MORE )      // probable 4-byte codepoint
                    GpiSetColor( hps, CLR_DARKBLUE );
                else if ( flags & CPV_UCS_FAILED )  // conversion to UCS failed
                    GpiSetColor( hps, CLR_RED );
                else if ( flags & CPV_UPF_FAILED )  // conversion from UCS failed
                    GpiSetColor( hps, CLR_DARKRED );
            }

            // Get the current cell rectangle
            rclCell.xLeft   = xCurrent + 1;
            rclCell.yBottom = yCurrent + 1;
            rclCell.xRight  = xCurrent + coord.xCellWidth - 1;
            rclCell.yTop    = yCurrent + coord.yCellHeight - 1;

            if ( fFill ) {
                ptl.x = rclCell.xLeft;
                ptl.y = rclCell.yBottom;
                GpiMove( hps, &ptl );
                ptl.x += coord.xCellWidth - 2;
                ptl.y += coord.yCellHeight - 2;
                GpiBox( hps, DRO_FILL, &ptl, 0, 0 );
                GpiSetColor( hps, CLR_PALEGRAY );
            }
            ptl.x = xCurrent + ( coord.xCellWidth  / 2 );
            ptl.y = yCurrent + yTextOffset;
            pszOutput = (fUnicode || global.fUCS) ? global.szOutput3[ ucVal ] : szCpVal;
            GpiCharStringPosAt( hps, &ptl, &rclCell, CHS_CLIP,
                                strlen(pszOutput), pszOutput, NULL );
            xCurrent += coord.xCellWidth;
        }
    }
}


/* ------------------------------------------------------------------------- *
 * PaintChars4B                                                              *
 *                                                                           *
 * Populate the character cells within a level 4 (4-byte) ward.  Note that   *
 * since only one codepage (964, a.k.a. EUC-TW) actually supports 4-byte     *
 * characters, we can skip a few of the checks we did in PaintChars2B & -3B. *
 *                                                                           *
 * ARGUMENTS:                                                                *
 *   HPS   hps        : Handle to the client window's presentation space.    *
 *   RECTL rcl        : The current boundaries of the client window.         *
 *   LONG  yTextOffset: The current vertical text offset within a cell.      *
 *                                                                           *
 * RETURNS: N/A                                                              *
 * ------------------------------------------------------------------------- */
void PaintChars4B( HPS hps, RECTL rcl, LONG yTextOffset, BOOL fUnicode )
{
    RECTL  rclCell;        // clipping rectangle for the current cell
    POINTL ptl;            // used for GPI positioning
    LONG   xCurrent,       // current X coordinate (left of current cell)
           yCurrent;       // current Y coordinate (bottom of current cell)
    UCHAR  row, col,       // row, column in 16x16 byte value grid
           ucVal;          // the current character's byte value
    CHAR   szCpVal[ 5 ];   // current character as a native codepage string
    PSZ    pszOutput;      // pointer to rendered character string
    BOOL   fFill;          // indicates whether cell should be shaded
    ULONG  flags;

    yCurrent = rcl.yTop - coord.yMargin - coord.yHeadHeight;
    for ( row = 0; row < 16; row++ ) {
        xCurrent = coord.xMargin + coord.xHeadWidth;
        yCurrent -= coord.yCellHeight;
        for ( col = 0; col < 16; col++ ) {
            ucVal = ( 16 * row ) + col;
            szCpVal[ 0 ] = global.ucFirst;
            szCpVal[ 1 ] = global.ucSecond;
            szCpVal[ 2 ] = global.ucThird;
            szCpVal[ 3 ] = ucVal;
            szCpVal[ 4 ] = '\0';

            // See if we need to colour in the cell.
            flags = codepage.flValues4[ ucVal ];
            if ( !flags ) {                         // displayable character
                GpiSetColor( hps, CLR_DEFAULT );
                fFill = FALSE;
            } else {
                fFill = TRUE;
                if ( flags & CPV_CODEPOINT )        // illegal value
                    GpiSetColor( hps, CLR_PALEGRAY );
                else if ( flags & CPV_UCS_FAILED )  // conversion to UCS failed
                    GpiSetColor( hps, CLR_RED );
                else if ( flags & CPV_UPF_FAILED )  // conversion from UCS failed
                    GpiSetColor( hps, CLR_DARKRED );
            }

            // Get the current cell rectangle
            rclCell.xLeft   = xCurrent + 1;
            rclCell.yBottom = yCurrent + 1;
            rclCell.xRight  = xCurrent + coord.xCellWidth - 1;
            rclCell.yTop    = yCurrent + coord.yCellHeight - 1;

            if ( fFill ) {
                ptl.x = xCurrent + 1;
                ptl.y = yCurrent + 1;
                GpiMove( hps, &ptl );
                ptl.x += coord.xCellWidth - 2;
                ptl.y += coord.yCellHeight - 2;
                GpiBox( hps, DRO_FILL, &ptl, 0, 0 );
                GpiSetColor( hps, CLR_PALEGRAY );
            }
            ptl.x = xCurrent + ( coord.xCellWidth  / 2 );
            ptl.y = yCurrent + yTextOffset;
            pszOutput = (fUnicode || global.fUCS) ? global.szOutput4[ ucVal ] : szCpVal;
            GpiCharStringPosAt( hps, &ptl, &rclCell, CHS_CLIP,
                                strlen(pszOutput), pszOutput, NULL );
            xCurrent += coord.xCellWidth;
        }
    }
}


/* ------------------------------------------------------------------------- *
 * ProcessClick                                                              *
 *                                                                           *
 * Handle mouse (button-1) clicks in the client-window area.                 *
 * ------------------------------------------------------------------------- */
void ProcessClick( HWND hwnd, SHORT ptx, SHORT pty )
{
    RECTL  rcl;
    SHORT  row, col;
    LONG   xLeftEdge, yBottomEdge, xRightEdge, yTopEdge;
    UCHAR  ucVal;
    CHAR   szTitle[ US_TITLE_MAX ];


    // Work out some basic measurements
    WinQueryWindowRect( hwnd, &rcl );
    xLeftEdge   = coord.xMargin + coord.xHeadWidth + 1;
    xRightEdge  = xLeftEdge + (16 * coord.xCellWidth);
    yTopEdge    = rcl.yTop - (coord.yMargin + coord.yHeadHeight) + 1;
    yBottomEdge = yTopEdge - (16 * coord.yCellHeight);

    // If user clicked outside the grid, go back up a level
    if ( ptx < xLeftEdge || ptx > xRightEdge || pty < yBottomEdge || pty > yTopEdge ) {
        switch ( global.fCharMode ) {
            case CH_MODE_DOUBLE:
                // Revert to level 1
                global.fCharMode = CH_MODE_SINGLE;
                global.ucFirst   = 0;
                if ( global.ulCP == 1 )
                    sprintf( szTitle, "Viewing OS2UGL - %s", global.szFace );
                else
                    sprintf( szTitle, "Viewing Codepage %d - %s", global.ulCP, global.szFace );
                WinSetWindowText( WinQueryWindow( hwnd, QW_PARENT ), szTitle );
                WinInvalidateRegion( hwnd, NULLHANDLE, TRUE );
                break;
            case CH_MODE_TRIPLE:
                // Revert to level 2 (2-byte display)
                global.fCharMode = CH_MODE_DOUBLE;
                global.ucSecond  = 0;
                if ( global.ulCP == 1 )
                    sprintf( szTitle, "Viewing OS2UGL [0x%02X-] - %s", global.ucFirst, global.szFace );
                else
                    sprintf( szTitle, "Viewing Codepage %d [0x%02X-] - %s", global.ulCP, global.ucFirst, global.szFace );
                WinSetWindowText( WinQueryWindow( hwnd, QW_PARENT ), szTitle );
                WinInvalidateRegion( hwnd, NULLHANDLE, TRUE );
                break;
            case CH_MODE_QUADRPL:
                // Revert to level 3 (3-byte display)
                global.fCharMode = CH_MODE_TRIPLE;
                global.ucThird   = 0;
                sprintf( szTitle, "Viewing Codepage %d [0x%02X%02X-] - %s",
                                  global.ulCP, global.ucFirst, global.ucSecond, global.szFace );
                WinSetWindowText( WinQueryWindow( hwnd, QW_PARENT ), szTitle );
                WinInvalidateRegion( hwnd, NULLHANDLE, TRUE );
                break;
            default: break;
        }
        return;
    }

    // Figure out which grid cell we're in
    col = (ptx - xLeftEdge) / coord.xCellWidth;
    row = 0xF - ((pty - yBottomEdge) / coord.yCellHeight);
    if ( col < 0 || col > 15 || row < 0 || row > 15 )
        return;

    // Get the current byte value
    ucVal = ( 16 * row ) + col;

    // Clicked in a cell: figure out what action to take, if any
    switch ( global.fCharMode ) {
        case CH_MODE_SINGLE:
            if ( ISLEADINGBYTE( codepage.fByteType[ ucVal ]) ) {
                // Switch to level 2 (2-byte character) display
                global.fCharMode = CH_MODE_DOUBLE;
                global.ucFirst   = ucVal;
                GenerateOutputLvl2( ucVal );
                if ( global.ulCP == 1 )
                    sprintf( szTitle, "Viewing OS2UGL [0x%02X-] - %s", ucVal, global.szFace );
                else
                    sprintf( szTitle, "Viewing Codepage %d [0x%02X-] - %s", global.ulCP, global.ucFirst, global.szFace );
                WinSetWindowText( WinQueryWindow( hwnd, QW_PARENT ), szTitle );
                WinInvalidateRegion( hwnd, NULLHANDLE, TRUE );
            }
            break;
        case CH_MODE_DOUBLE:
            if (( codepage.flValues2[ ucVal ] & CPV_1_MORE ) ||
                ( codepage.flValues2[ ucVal ] & CPV_2_MORE ))
            {
                // Switch to level 3 (3-byte character) display
                global.fCharMode = CH_MODE_TRIPLE;
                global.ucSecond  = ucVal;
                GenerateOutputLvl3( global.ucFirst, ucVal );
                sprintf( szTitle, "Viewing Codepage %d [0x%02X%02X-] - %s",
                                  global.ulCP, global.ucFirst, global.ucSecond, global.szFace );
                WinSetWindowText( WinQueryWindow( hwnd, QW_PARENT ), szTitle );
                WinInvalidateRegion( hwnd, NULLHANDLE, TRUE );
            }
            break;
        case CH_MODE_TRIPLE:
            if ( codepage.flValues3[ ucVal ] & CPV_1_MORE ) {
                // Switch to level 4 (4-byte character) display
                global.fCharMode = CH_MODE_QUADRPL;
                global.ucThird   = ucVal;
                GenerateOutputLvl4( global.ucFirst, global.ucSecond, ucVal );
                sprintf( szTitle, "Viewing Codepage %d [0x%02X%02X%02X-] - %s",
                                  global.ulCP, global.ucFirst, global.ucSecond, global.ucThird, global.szFace );
                WinSetWindowText( WinQueryWindow( hwnd, QW_PARENT ), szTitle );
                WinInvalidateRegion( hwnd, NULLHANDLE, TRUE );
            }
            break;
        default:
            break;
    }

}


/* ------------------------------------------------------------------------- *
 * MetricsDlgProc                                                            *
 *                                                                           *
 * Dialog procedure for the font information dialog.                         *
 * ------------------------------------------------------------------------- */
MRESULT EXPENTRY MetricsDlgProc( HWND hwnd, ULONG msg, MPARAM mp1, MPARAM mp2 )
{
    CHAR            szTitle[ US_TITLE_MAX ];
    HWND            hwndCnr;
    CNRINFO         cnr;
    PFIELDINFO      pFld,
                    pFld1st;
    FIELDINFOINSERT finsert;
    PFIRECORD       pRec;

    switch ( msg ) {

        case WM_INITDLG:
            // set the window title to show the current font
            sprintf( szTitle, "Font Information - %s", global.szFace );
            WinSetWindowText( hwnd, szTitle );

            // set up the font information container
            hwndCnr = WinWindowFromID( hwnd, IDD_FONTDATA );
            memset( &cnr, 0, sizeof( CNRINFO ));
            cnr.flWindowAttr  = CV_DETAIL | CA_DETAILSVIEWTITLES;
            cnr.pszCnrTitle   = "Font Metrics";
            cnr.cyLineSpacing = 1;
            WinSendMsg( hwndCnr, CM_SETCNRINFO, MPFROMP( &cnr ),
                        MPFROMLONG( CMA_FLWINDOWATTR | CMA_LINESPACING ));
            pFld = (PFIELDINFO) WinSendMsg( hwndCnr,
                                            CM_ALLOCDETAILFIELDINFO,
                                            MPFROMLONG( 3L ), 0 );
            pFld1st = pFld;
            // (first column: font information item name)
            pFld->cb = sizeof( FIELDINFO );
            pFld->pTitleData = "Attribute";
            pFld->flData     = CFA_STRING | CFA_FIREADONLY | CFA_VCENTER | CFA_HORZSEPARATOR | CFA_SEPARATOR;
            pFld->offStruct  = FIELDOFFSET( FIRECORD, pszItemName );
            pFld = pFld->pNextFieldInfo;
            // (second column: font information item value)
            pFld->cb = sizeof( FIELDINFO );
            pFld->pTitleData = "Value";
            pFld->flData     = CFA_STRING | CFA_FIREADONLY | CFA_VCENTER | CFA_HORZSEPARATOR | CFA_SEPARATOR;
            pFld->offStruct  = FIELDOFFSET( FIRECORD, pszItemValue );
            pFld = pFld->pNextFieldInfo;
            // (third column: font information item value explanation)
            pFld->cb = sizeof( FIELDINFO );
            pFld->pTitleData = "Notes";
            pFld->flData     = CFA_STRING | CFA_FIREADONLY | CFA_VCENTER | CFA_HORZSEPARATOR;
            pFld->offStruct  = FIELDOFFSET( FIRECORD, pszItemDetails );
            finsert.cb                   = (ULONG) sizeof( FIELDINFOINSERT );
            finsert.pFieldInfoOrder      = (PFIELDINFO) CMA_END;
            finsert.fInvalidateFieldInfo = TRUE;
            finsert.cFieldInfoInsert     = 3;
            WinSendMsg( hwndCnr, CM_INSERTDETAILFIELDINFO,
                        MPFROMP( pFld1st ), MPFROMP( &finsert ));
            PopulateFontDetails( hwndCnr );

            CentreWindow( hwnd );
            WinShowWindow( hwnd, TRUE );
            break;


        case WM_DESTROY:
            // free the allocated container memory
            pRec = (PFIRECORD) WinSendDlgItemMsg( hwnd, IDD_FONTDATA,
                                                  CM_QUERYRECORD, NULL,
                                                  MPFROM2SHORT( CMA_FIRST,
                                                                CMA_ITEMORDER ));
            while ( pRec ) {
                free( pRec->pszItemName );
                free( pRec->pszItemValue );
                if ( pRec->pszItemDetails ) free( pRec->pszItemDetails );
                pRec = (PFIRECORD) pRec->record.preccNextRecord;
            }
            WinSendDlgItemMsg( hwnd, IDD_FONTDATA, CM_REMOVERECORD, NULL,
                               MPFROM2SHORT( 0, CMA_INVALIDATE | CMA_FREE ));
            WinSendDlgItemMsg( hwnd, IDD_FONTDATA, CM_REMOVEDETAILFIELDINFO, NULL,
                               MPFROM2SHORT( 0, CMA_INVALIDATE | CMA_FREE ));
            break;


        default: break;
    }

    return WinDefDlgProc( hwnd, msg, mp1, mp2 );
}


/* ------------------------------------------------------------------------- *
 * PopulateFontDetails                                                       *
 *                                                                           *
 * Populate the font details container on the font information dialog.       *
 * ------------------------------------------------------------------------- */
void PopulateFontDetails( HWND hwndCnr )
{
    HPS          hps;
    LONG         lCount;
    ULONG        ulCB;
    USHORT       us;
    FONTMETRICS  fm;
    PFIRECORD    pRec, pFirst;
    RECORDINSERT ri;

    // get the current font information (metrics)
    hps = WinGetScreenPS( HWND_DESKTOP );
    memset( &fm, 0, sizeof( FONTMETRICS ));
    lCount = 1;
    GpiQueryFonts( hps, QF_PUBLIC, global.szFace,
                   &lCount, sizeof( FONTMETRICS ), &fm );
    WinReleasePS( hps );

    // now populate the container with the font information
    ulCB = sizeof( FIRECORD ) - sizeof( MINIRECORDCORE );
    pRec = (PFIRECORD) WinSendMsg( hwndCnr, CM_ALLOCRECORD,
                                   MPFROMLONG( ulCB ),
                                   MPFROMLONG( US_FM_RECORDS ));
    pFirst = pRec;
    ulCB = sizeof( MINIRECORDCORE );

    pRec->pszItemName    = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue   = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Family name");
    sprintf( pRec->pszItemValue, "%.31s", fm.szFamilyname );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName    = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue   = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Face name");
    sprintf( pRec->pszItemValue, "%.31s", fm.szFacename );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "IBM registered ID");
    sprintf( pRec->pszItemValue, "%u", fm.idRegistry );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Codepage");
    sprintf( pRec->pszItemValue, "%u", fm.usCodePage );
    switch ( fm.usCodePage ) {
        case 0:     strcpy( pRec->pszItemDetails,  "Any");                 break;
        case 65400: strcpy( pRec->pszItemDetails,  "None (symbol font)");  break;
        default:    sprintf( pRec->pszItemDetails, "Codepage %u only", fm.usCodePage );
    }
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Em height");
    sprintf( pRec->pszItemValue, "%d", fm.lEmHeight );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "X height");
    sprintf( pRec->pszItemValue, "%d", fm.lXHeight );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Max ascender");
    sprintf( pRec->pszItemValue, "%d", fm.lMaxAscender );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Max descender");
    sprintf( pRec->pszItemValue, "%d", fm.lMaxDescender );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Lowercase ascent");
    sprintf( pRec->pszItemValue, "%d", fm.lLowerCaseAscent );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Lowercase descent");
    sprintf( pRec->pszItemValue, "%d", fm.lLowerCaseDescent );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Internal leading");
    sprintf( pRec->pszItemValue, "%d", fm.lInternalLeading );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "External leading");
    sprintf( pRec->pszItemValue, "%d", fm.lExternalLeading );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Average character width");
    sprintf( pRec->pszItemValue, "%d", fm.lAveCharWidth );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Max character increment");
    sprintf( pRec->pszItemValue, "%d", fm.lMaxCharInc );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Em increment");
    sprintf( pRec->pszItemValue, "%d", fm.lEmInc );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Max baseline extent");
    sprintf( pRec->pszItemValue, "%d", fm.lMaxBaselineExt );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Character slope");
    sprintf( pRec->pszItemValue, "%d", fm.sCharSlope );
    // not a UniChar, but BYTE* macros are useful for grabbing individual bytes
    sprintf( pRec->pszItemDetails, "%d degrees, %d minutes",
             BYTE2FROMUNIC( fm.sCharSlope ), BYTE1FROMUNIC( fm.sCharSlope ));
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Inline direction");
    sprintf( pRec->pszItemValue, "%d", fm.sInlineDir );
    sprintf( pRec->pszItemDetails, "%d degrees, %d minutes",
             BYTE2FROMUNIC( fm.sInlineDir ), BYTE1FROMUNIC( fm.sInlineDir ));
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Character rotation");
    sprintf( pRec->pszItemValue, "%d", fm.sCharRot );
    sprintf( pRec->pszItemDetails, "%d degrees, %d minutes",
             BYTE2FROMUNIC( fm.sCharRot ), BYTE1FROMUNIC( fm.sCharRot ));
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Weight class");
    sprintf( pRec->pszItemValue, "%u", fm.usWeightClass );
    switch ( fm.usWeightClass ) {
        case 1:  strcpy( pRec->pszItemDetails, "Ultra-light");      break;
        case 2:  strcpy( pRec->pszItemDetails, "Extra-light");      break;
        case 3:  strcpy( pRec->pszItemDetails, "Light");            break;
        case 4:  strcpy( pRec->pszItemDetails, "Semi-light");       break;
        case 5:  strcpy( pRec->pszItemDetails, "Medium (normal)");  break;
        case 6:  strcpy( pRec->pszItemDetails, "Semi-bold");        break;
        case 7:  strcpy( pRec->pszItemDetails, "Bold");             break;
        case 8:  strcpy( pRec->pszItemDetails, "Extra-bold");       break;
        case 9:  strcpy( pRec->pszItemDetails, "Ultra-bold");       break;
        default: strcpy( pRec->pszItemDetails, "Unknown weight class");
    }
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Width class");
    sprintf( pRec->pszItemValue, "%d", fm.usWidthClass );
    switch ( fm.usWidthClass ) {
        case 1:  strcpy( pRec->pszItemDetails, "Ultra-condensed (50%)");   break;
        case 2:  strcpy( pRec->pszItemDetails, "Extra-condensed (62.5%)"); break;
        case 3:  strcpy( pRec->pszItemDetails, "Condensed (75%)");         break;
        case 4:  strcpy( pRec->pszItemDetails, "Semi-condensed (87.5%)");  break;
        case 5:  strcpy( pRec->pszItemDetails, "Normal (100%)");           break;
        case 6:  strcpy( pRec->pszItemDetails, "Semi-expanded (112.5%)");  break;
        case 7:  strcpy( pRec->pszItemDetails, "Expanded (125%)");         break;
        case 8:  strcpy( pRec->pszItemDetails, "Extra-expanded (150%)");   break;
        case 9:  strcpy( pRec->pszItemDetails, "Ultra-expanded (200%)");   break;
        default: strcpy( pRec->pszItemDetails, "Unknown width class");
    }
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    // The X and Y resolution fields are combined together in one record
    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Device resolution");
    sprintf( pRec->pszItemValue, "%d x %d", fm.sXDeviceRes, fm.sYDeviceRes );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "First character");
    sprintf( pRec->pszItemValue, "%u", fm.sFirstChar );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Last character");
    if ( fm.fsType & FM_TYPE_UNICODE )
        sprintf( pRec->pszItemValue, "U+%X", (USHORT) fm.sLastChar + fm.sFirstChar );
    else
        sprintf( pRec->pszItemValue, "%d", fm.sLastChar + fm.sFirstChar );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Default character");
    sprintf( pRec->pszItemValue, "%d", fm.sDefaultChar + fm.sFirstChar );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Break character");
    sprintf( pRec->pszItemValue, "%d", fm.sBreakChar + fm.sFirstChar );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Nominal point size");
    sprintf( pRec->pszItemValue, "%d", fm.sNominalPointSize );
    sprintf( pRec->pszItemDetails, "%d pt", fm.sNominalPointSize / 10 );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Minimum point size");
    sprintf( pRec->pszItemValue, "%d", fm.sMinimumPointSize );
    sprintf( pRec->pszItemDetails, "%d pt", fm.sMinimumPointSize / 10 );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Maximum point size");
    sprintf( pRec->pszItemValue, "%d", fm.sMaximumPointSize );
    sprintf( pRec->pszItemDetails, "%d pt", fm.sMaximumPointSize / 10 );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Type flags");
    sprintf( pRec->pszItemValue, "0x%X", fm.fsType );
    pRec->pszItemDetails[0] = '\0';
    if ( fm.fsType & FM_TYPE_FIXED )
        strncat( pRec->pszItemDetails, "Fixed-width / ", US_FMDESC_MAX );
    if ( fm.fsType & FM_TYPE_LICENSED )
        strncat( pRec->pszItemDetails, "Licensed / ", US_FMDESC_MAX );
    if ( fm.fsType & FM_TYPE_KERNING )
        strncat( pRec->pszItemDetails, "Kerning / ", US_FMDESC_MAX );
    if ( fm.fsType & FM_TYPE_64K )
        strncat( pRec->pszItemDetails, "Larger than 64KB / ", US_FMDESC_MAX );
    if ( fm.fsType & FM_TYPE_MBCS )
        strncat( pRec->pszItemDetails, "MBCS / ", US_FMDESC_MAX );
    else if ( fm.fsType & FM_TYPE_DBCS )
        strncat( pRec->pszItemDetails, "DBCS / ", US_FMDESC_MAX );
    if ( fm.fsType & FM_TYPE_UNICODE )
        strncat( pRec->pszItemDetails, "Unicode / ", US_FMDESC_MAX );
    if ( fm.fsType & FM_TYPE_ATOMS )
        strncat( pRec->pszItemDetails, "Valid atoms / ", US_FMDESC_MAX );
    if ( fm.fsType & FM_TYPE_FAMTRUNC )
        strncat( pRec->pszItemDetails, "Family name truncated / ", US_FMDESC_MAX );
    if ( fm.fsType & FM_TYPE_FACETRUNC )
        strncat( pRec->pszItemDetails, "Face name truncated / ", US_FMDESC_MAX );
    us = strlen( pRec->pszItemDetails );
    if ( us )
        pRec->pszItemDetails[ us-3 ] = '\0';
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Definition flags");
    sprintf( pRec->pszItemValue, "0x%X", fm.fsDefn );
    pRec->pszItemDetails[0] = '\0';
    if ( fm.fsDefn & FM_DEFN_OUTLINE )
        strncat( pRec->pszItemDetails, "Outline / ", US_FMDESC_MAX );
    else
        strncat( pRec->pszItemDetails, "Raster / ", US_FMDESC_MAX );
    if ( fm.fsDefn & FM_DEFN_IFI )
        strncat( pRec->pszItemDetails, "IFI / ", US_FMDESC_MAX );
    if ( fm.fsDefn & FM_DEFN_WIN )
        strncat( pRec->pszItemDetails, "WIN / ", US_FMDESC_MAX );
    if ( fm.fsDefn & 0x8 )             // this is just a guess
        strncat( pRec->pszItemDetails, "CMB / ", US_FMDESC_MAX );
    if ( fm.fsDefn & FM_DEFN_GENERIC )
        strncat( pRec->pszItemDetails, "Generic / ", US_FMDESC_MAX );
    else
        strncat( pRec->pszItemDetails, "Device / ", US_FMDESC_MAX );
    if ( fm.fsDefn & FM_DEFN_LATIN1 )
        strncat( pRec->pszItemDetails, "Latin-1, ", US_FMDESC_MAX );
    if ( fm.fsDefn & FM_DEFN_PC )
        strncat( pRec->pszItemDetails, "PC, ", US_FMDESC_MAX );
    if ( fm.fsDefn & FM_DEFN_LATIN2 )
        strncat( pRec->pszItemDetails, "Latin-2, ", US_FMDESC_MAX );
    if ( fm.fsDefn & FM_DEFN_CYRILLIC )
        strncat( pRec->pszItemDetails, "Cyrillic, ", US_FMDESC_MAX );
    if ( fm.fsDefn & FM_DEFN_HEBREW )
        strncat( pRec->pszItemDetails, "Hebrew, ", US_FMDESC_MAX );
    if ( fm.fsDefn & FM_DEFN_GREEK )
        strncat( pRec->pszItemDetails, "Greek, ", US_FMDESC_MAX );
    if ( fm.fsDefn & FM_DEFN_ARABIC )
        strncat( pRec->pszItemDetails, "Arabic, ", US_FMDESC_MAX );
    if ( fm.fsDefn & FM_DEFN_UGLEXT )
        strncat( pRec->pszItemDetails, "UGL-Extra, ", US_FMDESC_MAX );
    if ( fm.fsDefn & FM_DEFN_KANA )
        strncat( pRec->pszItemDetails, "Kana, ", US_FMDESC_MAX );
    if ( fm.fsDefn & FM_DEFN_THAI )
        strncat( pRec->pszItemDetails, "Thai, ", US_FMDESC_MAX );
    us = strlen( pRec->pszItemDetails );
    if ( us )
        pRec->pszItemDetails[ us-2 ] = '\0';
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Selection flags");
    sprintf( pRec->pszItemValue, "0x%X", fm.fsSelection );
    pRec->pszItemDetails[0] = '\0';
    if ( fm.fsSelection & FM_SEL_ITALIC )
        strncat( pRec->pszItemDetails, "Italic / ", US_FMDESC_MAX );
    if ( fm.fsSelection & FM_SEL_UNDERSCORE )
        strncat( pRec->pszItemDetails, "Underscore / ", US_FMDESC_MAX );
    if ( fm.fsSelection & FM_SEL_NEGATIVE )
        strncat( pRec->pszItemDetails, "Negative / ", US_FMDESC_MAX );
    if ( fm.fsSelection & FM_SEL_OUTLINE )
        strncat( pRec->pszItemDetails, "Outlined / ", US_FMDESC_MAX );
    if ( fm.fsSelection & FM_SEL_STRIKEOUT )
        strncat( pRec->pszItemDetails, "Strikeout / ", US_FMDESC_MAX );
    if ( fm.fsSelection & FM_SEL_BOLD )
        strncat( pRec->pszItemDetails, "Bold / ", US_FMDESC_MAX );
    if ( fm.fsSelection & FM_SEL_ISO9241_TESTED )
        strncat( pRec->pszItemDetails, "ISO-9241 / ", US_FMDESC_MAX );
    if ( fm.fsSelection & FM_SEL_JAPAN )
        strncat( pRec->pszItemDetails, "Japan / ", US_FMDESC_MAX );
    if ( fm.fsSelection & FM_SEL_TAIWAN )
        strncat( pRec->pszItemDetails, "Taiwan / ", US_FMDESC_MAX );
    if ( fm.fsSelection & FM_SEL_CHINA )
        strncat( pRec->pszItemDetails, "China / ", US_FMDESC_MAX );
    if ( fm.fsSelection & FM_SEL_KOREA )
        strncat( pRec->pszItemDetails, "Korea / ", US_FMDESC_MAX );
    us = strlen( pRec->pszItemDetails );
    if ( us )
        pRec->pszItemDetails[ us-3 ] = '\0';
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Font capabilities");
    sprintf( pRec->pszItemValue, "0x%X", fm.fsCapabilities );
    pRec->pszItemDetails[0] = '\0';
    if ( fm.fsCapabilities & FM_CAP_NOMIX )
        strncat( pRec->pszItemDetails, "NoMix / ", US_FMDESC_MAX );
    if ( fm.fsCapabilities & FM_CAP_NO_COLOR )
        strncat( pRec->pszItemDetails, "NoColor / ", US_FMDESC_MAX );
    if ( fm.fsCapabilities & FM_CAP_NO_MIXEDMODES )
        strncat( pRec->pszItemDetails, "NoMixedModes / ", US_FMDESC_MAX );
    if ( fm.fsCapabilities & FM_CAP_NO_HOLLOW )
        strncat( pRec->pszItemDetails, "NoHollow / ", US_FMDESC_MAX );
    if ( fm.fsCapabilities & 0x10 )
        strncat( pRec->pszItemDetails, "DP quality / ", US_FMDESC_MAX );
    if ( fm.fsCapabilities & 0x20 )
        strncat( pRec->pszItemDetails, "DP draft / ", US_FMDESC_MAX );
    if ( fm.fsCapabilities & 0x40 )
        strncat( pRec->pszItemDetails, "Near letter quality / ", US_FMDESC_MAX );
    if ( fm.fsCapabilities & 0x80 )
        strncat( pRec->pszItemDetails, "Letter quality / ", US_FMDESC_MAX );
    us = strlen( pRec->pszItemDetails );
    if ( us )
        pRec->pszItemDetails[ us-3 ] = '\0';
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    // Subscript X and Y size values are combined together in one record
    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Subscript size");
    sprintf( pRec->pszItemValue, "%d x %d", fm.lSubscriptXSize, fm.lSubscriptYSize );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    // Subscript X and Y offset values are combined together in one record
    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Subscript offset");
    sprintf( pRec->pszItemValue, "%d, %d", fm.lSubscriptXOffset, fm.lSubscriptYOffset );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    // Superscript X and Y size values are combined together in one record
    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Superscript size");
    sprintf( pRec->pszItemValue, "%d x %d", fm.lSuperscriptXSize, fm.lSuperscriptYSize );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    // Superscript X and Y offset values are combined together in one record
    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Superscript offset");
    sprintf( pRec->pszItemValue, "%d, %d", fm.lSuperscriptXOffset, fm.lSuperscriptYOffset );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Underscore size");
    sprintf( pRec->pszItemValue, "%d", fm.lUnderscoreSize );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Underscore position");
    sprintf( pRec->pszItemValue, "%d", fm.lUnderscorePosition );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Strikeout size");
    sprintf( pRec->pszItemValue, "%d", fm.lStrikeoutSize );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Strikeout position");
    sprintf( pRec->pszItemValue, "%d", fm.lStrikeoutPosition );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Kerning pairs");
    sprintf( pRec->pszItemValue, "%d", fm.sKerningPairs );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = (PSZ) malloc( US_FMDESC_MAX );
    strcpy( pRec->pszItemName, "Font family classification");
    sprintf( pRec->pszItemValue, "%d", fm.sFamilyClass );
    sprintf( pRec->pszItemDetails, "Class %d, Subclass %d",
             BYTE2FROMUNIC( fm.sFamilyClass ), BYTE1FROMUNIC( fm.sFamilyClass ));
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Matched font identity");
    sprintf( pRec->pszItemValue, "%d", fm.lMatch );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Font family name atom");
    sprintf( pRec->pszItemValue, "%d", fm.FamilyNameAtom );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    pRec->pszItemDetails = NULL;
    strcpy( pRec->pszItemName, "Font face name atom");
    sprintf( pRec->pszItemValue, "%d", fm.FaceNameAtom );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;
    pRec = (PFIRECORD) pRec->record.preccNextRecord;

    pRec->pszItemName = (PSZ) malloc( US_FMITEM_MAX );
    pRec->pszItemValue = (PSZ) malloc( US_FMVAL_MAX );
    strcpy( pRec->pszItemName, "Panose font descriptor");
    sprintf( pRec->pszItemValue, "{%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,0x%X,0x%X}",
                                  fm.panose.bFamilyType,
                                  fm.panose.bSerifStyle,
                                  fm.panose.bWeight,
                                  fm.panose.bProportion,
                                  fm.panose.bContrast,
                                  fm.panose.bStrokeVariation,
                                  fm.panose.bArmStyle,
                                  fm.panose.bLetterform,
                                  fm.panose.bMidline,
                                  fm.panose.bXHeight,
                                  fm.panose.fbPassedISO,
                                  fm.panose.fbFailedISO );
    pRec->record.cb = ulCB;
    pRec->record.pszIcon = pRec->pszItemValue;

    // Now insert the records
    ri.cb                = sizeof( RECORDINSERT );
    ri.pRecordOrder      = (PRECORDCORE) CMA_END;
    ri.pRecordParent     = NULL;
    ri.zOrder            = (ULONG) CMA_TOP;
    ri.fInvalidateRecord = TRUE;
    ri.cRecordsInsert    = US_FM_RECORDS;
    WinSendMsg( hwndCnr, CM_INSERTRECORD, MPFROMP( pFirst ), MPFROMP( &ri ));

}
