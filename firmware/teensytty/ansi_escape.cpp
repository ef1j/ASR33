/*
 * Copyright (c) Hugh Pyle
 * 
 * ansi_escape.cpp
 * Implement a subset of vt100 escape sequences to control the teletype ASR33.
 * - Cursor positioning along the horizontal line;
 * - Auto-wrap at the end of the line;
 * - Application codes to re
 * - Identification and reset.
 * 
 * https://vt100.net/docs/vt220-rm/contents.html
 * http://www.inwap.com/pdp10/ansicode.txt
 * https://docs.microsoft.com/en-us/windows/console/console-virtual-terminal-sequences
 * https://www.proteansec.com/linux/blast-past-executing-code-terminal-emulators-via-escape-sequences/
 *
 */

#include "ansi_escape.h"


#define MAX_COLUMNS 71
#define ESC '\033'
#define NL  '\n'
#define CR  '\r'

// a line full of spaces
#define SPACES    "                                                                           "
#define CR_SPACES "\r                                                                         "


AnsiEscapeProcessor::AnsiEscapeProcessor() {
  init();
}

void AnsiEscapeProcessor::init() {
  escState = escStateNone;
  nEscChars = 0;
  col = 0;
  isEscSimple = false;
  isEscApc = false;
  isEscCsi = false;
  isCsiQuestion = false;
  isWrapping  = false;
  isNLCR      = true;
  isNulDelays = true;
  isSoftReset = false;
  isHardReset = false;

  pLen = (uint8_t *)outbuf;
  pBuf = pLen + 1;
}



int AnsiEscapeProcessor::column() {
  // what column are we at? (typically 0 thru 71)
  return col;
}


/*
process a single character;
return a length-prefixed buffer of characters that should be sent to the unit.
*/
uint8_t *AnsiEscapeProcessor::update(uint8_t c) {

    // Serial.printf("%c", c);

    // Put the character into the next slot of the buffer
    // and keep the buffer null-terminated so we can read it like a string
    *pBuf = c;
    *(pBuf+1) = 0;

    // Handle escape sequences
    
    switch(escState) {
        case escStateNone:
            // We're outside an escape sequence and received a character
            *pLen = 1; // length is the single 
            
            // If the character is ESC, start gathering.
            if(c==ESC) {
                escState = escStateEsc;
                nEscChars = 0;
                isEscSimple = false;
                isEscApc = false;
                isEscCsi = false;
                isCsiQuestion = false;
                // We're swallowing these, so the out-length is zero
                *pLen = 0;
            }
            break;

        case escStateEsc:
            // We're in an escape sequence and received a character
            nEscChars++;
            isEscSimple = (nEscChars==1);

            // We're swallowing these until terminated, so the out-length is zero
            *pLen = 0;

            // Is this a terminator?  If so, we're ending the sequence, let's process it
            if(isTerminator(c) || nEscChars>MAXESCLEN) {
                escState = escStateNone;
                
                // We finished now.
                processSequence();
            }

            if(nEscChars==1 && c=='_') {
              isEscApc = true;
            }
            if(nEscChars==1 && c=='[') {
              isEscCsi = true;
            }
            if(isEscCsi && nEscChars==2 && c=='?') {
              isCsiQuestion = true;
            }            
            break;

        case escStateEnd:
            break;
    }

    // Outside of escape sequences, handle the character stream
    // with whatever additional processing is needed
    
    if(escState == escStateNone) {
      pBuf = pLen + 1;
      
      if(isWrapping) {
        // We're wrapping lines.
        // If the line is too long, break it
        if(col==MAX_COLUMNS) {
          char buf[] = {'\r', '\n', c};
          writeOutput(buf, 3);
        }
      }

      if(isNLCR) {
        // NL should turn in to CR+NL
        // (for now, handled by caller)
      }

      if(isNulDelays) {
        // CR should be followed by a 2-character "delay NUL"
        // NL should be followed by a 1-character "delay NUL"
        // (for now, handled by caller)
      }

    } else {
      pBuf++;
    }
    
    updateFromOutput();
    return outbuf;
}


bool AnsiEscapeProcessor::isTerminator(uint8_t c) {
  if(isEscSimple) {
    // The single-character escapes can end with a number
    // e.g ESC7, ESC8
    return ((c>='A' && c<='Z') || (c>='a' && c<='z') || (c>='0' && c<='9'));
  } else if(isEscApc) {
    // APC ends with String Terminator 0x9C
    return (c==0x9C);
  } else {
    // CIS etc terminate on alphabet
    return ((c>='A' && c<='Z') || (c>='a' && c<='z'));
  }
}


void AnsiEscapeProcessor::writeOutput(const char *out, uint8_t n) {
  // Reset the output buffer to contain the first 'n' characters of *out
  pLen = (uint8_t *)outbuf;
  pBuf = pLen + 1;
  *pLen = n;
  strncpy((char*)pBuf, out, (size_t)n);
}


void AnsiEscapeProcessor::updateFromOutput() {
  // update the position and other properties
  // from the output-buffer
  // (there's no processing of escape sequences here, that already happened)
  uint8_t *p = (uint8_t *)outbuf;
  uint8_t len = *p++;

  for(uint8_t n=1; n<=len; n++) {
    uint8_t cc = *p++;
    updateFromChar(cc);
    //Serial.printf("%c", cc);
  }

}


void AnsiEscapeProcessor::updateFromChar(uint8_t c) {
    // Update our column-count based on a single character of output
    if(c==CR) {
      // CR puts us back at the left margin -- column zero
      col = 0;
    }
    else if(isPrintable(c)) {
      // printable updates the column position, unless we've hit the right margin
      if(col < MAX_COLUMNS) {
        col++;
      }
    }
}


// ------ Escape processing ------ //


void AnsiEscapeProcessor::processSequence() {
  // Buffer contains a complete sequence of escape characters
  // pBuf points at the last received character
  writeResponse(NULL);
  if(isEscSimple) {
    //Serial.printf("<ESC(%c)>", *pBuf);
    switch(*pBuf) {
      case 'B': //  CUD  Cursor Down by 1
        writeOutput("\n", 1);
        break;
      case 'C': // CUF Cursor Forward (Right) by 1
        moveToColumn(column() + 1);
        break;
      case 'D': // CUB Cursor Backward (Left) by 1
        moveToColumn(column() - 1);
        break;
      case '7': // DECSC Save Cursor Position in Memory
        savedCol = column();
        break;
      case '8': // DECSR Restore Cursor Position from Memory
        moveToColumn(savedCol);
        break;
      default:
        // Not Implemented
        break;
    }
  } else if(isEscApc) {
    // ESC _ <code> 0x9C -- we know *pBuf is 0x9C, process the characters in "code"
    readAPC();
  } else if(isEscCsi) {
    //Serial.printf("<ESC[>");
    if(!isCsiQuestion) {
      switch(*pBuf) {
        case 'B': // CUD  Cursor Down by N
          break;
        case 'C': // CUF Cursor Forward (Right) by N
          moveToColumn(column() + getN(1));
          break;
        case 'D': // CUB Cursor Backward (Left) by N
          moveToColumn(column() - getN(1));
          break;
        case 'G': // CHA Cursor Horizontal Absolute to N
          moveToColumn(getN(0));
          break;
        case 'I': // CHT Cursor Horizontal (Forward) Tab
          break;
        case 'Z': // CBT Cursor Backwards Tab
          break;
        case 'c': //  device attributes
          sendDA(getN(0));
          break;
        case 'n': //  device status report
          sendDSR(getN(0));
          break;
      }
    } else {
      // ESC [ ? ... 
      switch(*pBuf) {
        case 'h': //  set mode
          setMode(getN(0));
          break;
        case 'l': //  reset mode
          resetMode(getN(0));
          break;
        case 'n': //  device status report
          sendDSR(getN(0));
          break;
        case 'p': //  ESC[!p - soft terminal reset
          resetTerminal();
          break;
      }
    }
  } else {
    // Something we can't handle yet, ignore it
    //Serial.printf("<??>");
  }
}


int AnsiEscapeProcessor::getN(int defaultN) {
  // Get a single positional parameter integer <N> from (e.g.)
  //    ESC<N>x
  //    ESC[<N>x
  //    ESC[?<N>x

  // Buffer contains the escape sequence, but its length is nEscChars not in the prefix
  const char *p;

  // Skip the escape prefix
  if(isCsiQuestion) {
    p = (const char *)outbuf + 4;   // <len> <esc> [ ? <N>
  } else if(isEscCsi) {
    p = (const char *)outbuf + 3;   // <len> <esc> [ <N>
  } else {
    p = (const char *)outbuf + 2;   // <len> <esc> <N>
  }
  
  char* endptr = NULL;
  int n = (int)strtol(p, &endptr, 10);
  if (endptr == p) {
    // Nothing found
    n = defaultN;
  }
  //Serial.printf("{%d}", n);
  return n;
}


void AnsiEscapeProcessor::readAPC() {
  // Process an APC command consisting of zero or more bytes

  // Buffer contains <len> <esc> _ <command> 0x9C
  // Skip the escape prefix
  const char *p = (const char *)outbuf + 3;

  // Handle each byte of the command
  uint8_t n=0;
  while(*p != 0x9C && n < nEscChars) {
      switch(*p) {
        case APC_RX_NLCR_OFF:
          isNLCR = false;
          break;
        case APC_RX_NLCR_ON:
          isNLCR = true;
          break;
        case APC_RX_DELAYS_OFF:
          isNulDelays = false;
          break;
        case APC_RX_DELAYS_ON:
          isNulDelays = true;
          break;
      }
      n++;
      p++;
  }
}


void AnsiEscapeProcessor::moveToColumn(int n) {
  // Populate the output buffer with whatever we need to position at column 'n' (zero-based)
  // without any word-wrapping, and with a fixed line length of MAX_COLUMNS
  if(n<0) {
    // Can't go beyond the start of the line!
    n = 0;
  }
  if(n>MAX_COLUMNS) {
    // Can't go beyond the end of the line!
    n = MAX_COLUMNS;
  }
  if(n==col) {
    // nothing to do
    return;
  } else if(n>col) {
    // (n-col) spaces will move us forward
    writeOutput(SPACES, n-col);
  } else {
    // CR then (n) spaces
    writeOutput(CR_SPACES, n+1);
  }
}


void AnsiEscapeProcessor::setMode(int mode) {
  if(mode==7) {
    isWrapping = true;
  }
}


void AnsiEscapeProcessor::resetMode(int mode) {
  if(mode==7) {
    isWrapping = false;
  }  
}


void AnsiEscapeProcessor::resetTerminal() {
  init();
  isSoftReset = true;
  writeOutput("\r\n", 2);
}


// ------ Response to the host ------ //

uint8_t *AnsiEscapeProcessor::getResponse() {
  // The response that should be sent back to the host
  return pResponse;
}

void AnsiEscapeProcessor::writeResponse(const char *out) {
  // Set this as the response
  pResponse = (uint8_t *)out;
}

void AnsiEscapeProcessor::sendDA(int n) {
  switch(n) {
    case 0:
      // Status response to 'wtf are you?' is: 'I am a vt101 (bwahahaha)'
      writeResponse(IDENT_SEQUENCE);
  }
}

void AnsiEscapeProcessor::sendDSR(int n) {
  switch(n) {
    case 5:
      // DSR - Status response to 'r u awake?' is: 'I have no malfunction (bwahahaha)'
      writeResponse("\033[0n");
      break;
    case 15:
      // DSR - Status response to 'you print?' is: 'I have no printer (bwahahaha)'
      writeResponse("\033[?13n");
      break;
    case 6:
      // CPR - cursor position report
      int n = snprintf((char *)retbuf, 32, "\033[0;%dR", col);
      if(n>=0 && n<32) {
        writeResponse((char *)retbuf);
      }
  }
}



#ifdef ANSI_TESTING

// ------ Tests ------ //

void ansi_test(const char *input, int expect_col, const char *expect_rsp) {
  AnsiEscapeProcessor processor;
  int model_col;
  // Send the whole string to the processor
  for(uint i=0; i<strlen(input); i++) {
    uint8_t c = (uint8_t)input[i];
    uint8_t *p = processor.update(c);

    uint8_t len = *p++;
    //Serial.printf("%02x:", len);
    for(uint8_t n=1; n<=len; n++) {
      uint8_t cc = *p++;

      // send these to the physical tty
      // Serial.printf("%02x", cc);
    }
  }

  model_col = processor.column();
  if(model_col == expect_col) {
    Serial.printf(" Pass: %d\n", expect_col);
  } else {
    Serial.printf(" FAIL: %d != %d\n", model_col, expect_col);
  }

  if(expect_rsp!=NULL) {
    uint8_t *rsp = processor.getResponse();
    if(rsp==NULL) {
      Serial.printf(" Response FAIL: NULL != %s\n", expect_rsp);
    } else if(strcmp((char *)rsp, (char *)expect_rsp)!=0) {
      Serial.printf(" Response FAIL: %s != %s\n", rsp, expect_rsp);
    } else {
      Serial.printf(" Response PASS: %s\n", rsp);
    }
  }
}

void test_ansi(usb_serial_class& serial) {
  ansi_test("", 0, NULL);
  ansi_test("abc", 3, NULL);

  ansi_test("12345678901234567890123456789012345678901234567890123456789012345678901", 71, NULL);
  ansi_test("123456789012345678901234567890123456789012345678901234567890123456789012345", MAX_COLUMNS, NULL);

  // word wrap is like CRLF at the right place
  ansi_test("123456789 123456789 123456789 123456789 123456789 123456789 123456789 12\r\n3456789 123456789 123456789 123456789", 37, NULL);
  ansi_test("123456789 \033[?7h123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789", 38, NULL);
  ansi_test("123456789 \033[?7l123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789", MAX_COLUMNS, NULL);
  ansi_test("123456789 \033[?7h123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789 123456789", 7, NULL);
  ansi_test("123456789 \033[?7h123456789 123456789 123456789 123456789 123456789 123456789 123456789 \033[?7l123456789 123456789 123456789 123456789 123456789 123456789 123456789", MAX_COLUMNS, NULL);

  ansi_test("abc\nd", 4, NULL);
  ansi_test("abc\rd_", 2, NULL);
  ansi_test("abcd", 4, NULL);

  ansi_test("abc\0337defghijkl", 12, NULL); // ESC-7 "save state" in the middle has no effect on cursor position
  ansi_test("abc\0337def\0338",  3, NULL);  // ESC-8 "restore state" moves back to earlier position
  ansi_test("abc\0337def\0338g", 4, NULL);  // ESC-8 "restore state" moves back to earlier position

  ansi_test("abc\033Adefghijkl", 12, NULL); // ESC-A CUU not implemented
  ansi_test("abc\033Bdefghijkl", 12, NULL); // ESC-B CUD is just like lf
  ansi_test("abc\033Cdefghijkl", 13, NULL); // ESC-C CUF is like space
  ansi_test("abc\033Ddefghijkl", 11, NULL); // ESC-D CUB is like backspace
  ansi_test("\033Dabcdefghijkl", 12, NULL); // ESC-D can't go back from the beginning
  ansi_test("abc\033Mdefghijkl", 12, NULL); // ESC-M R   not implemented

  ansi_test("abc\033[Cdefghijkl", 13, NULL);  // ESC-C CUF is like space
  ansi_test("abc\033[Ddefghijkl", 11, NULL);  // ESC-D CUB is like backspace
  ansi_test("abc\033[2Cdefghijkl", 14, NULL); // ESC-C CUF is like space
  ansi_test("abc\033[2Ddefghijkl", 10, NULL); // ESC-D CUB is like backspace
  ansi_test("abc\033[5Ddefghijkl", 9, NULL);  // ESC-D can't go back that far from here
  ansi_test("abc\033[8Gdefghijkl", 17, NULL); // CHA Cursor Horizontal Absolute to N

  ansi_test("\033[c",  0, "\033[?1;0c");   // DA1
  ansi_test("\033[0c", 0, "\033[?1;0c");   // DA1
  ansi_test("\033[5n", 0, "\033[0n");      // DSR
  ansi_test("\033[?15n", 0, "\033[?13n");  // DSR

  // CPR rows always zero
  ansi_test("\033[6n", 0, "\033[0;0R");           // CPR when at (0,0)
  ansi_test("abc\033[6n", 3, "\033[0;3R");        // CPR when at (0,3)
  ansi_test("abc\rdefg\n\033[6n", 4, "\033[0;4R"); // CPR when at (<whatever>,4)

}

#endif
