// -*- c++ -*-
//
// Copyright 2010 Ovidiu Predescu <ovidiu@gmail.com>
// Date: May 2010
//
// Updated: 08-JAN-2012 for Arduno IDE 1.0 by <Hardcore@hardcoreforensics.com>
// Updated: 29-MAR-2013 replacing strtoul with parseHexChar by <shin@marcsi.ch>
//
// TinyWebServer for Arduino.
//
// The DEBUG flag will enable serial console logging in this library
// By default Debugging to the Serial console is OFF.
// This ensures that any scripts using the Serial port are not corrupted
// by the tinywebserver libraries debugging messages.
//
// To ENABLE debugging set the following:
// DEBUG 1 and ENSURE that you have configured the serial port in the
// main Arduino script.
//
// There is an overall size increase of about 340 bytes in code size
// when the debugging is enabled and debugging lines are preceded by 'TWS:'

#define DEBUG 0

// 10 milliseconds read timeout
#define READ_TIMEOUT 10

#include "Arduino.h"

extern "C" {

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
}

#include <Ethernet.h>
#include "Flash.h"
#ifdef _USE_SD_CARD
#include <SD.h>
#endif

#include "TinyWebServer.h"

// Temporary buffer.
static char buffer[160];

FLASH_STRING(mime_types,
  "HTM*text/html|"
  "TXT*text/plain|"
  "CSS*text/css|"
  "XML*text/xml|"
  "JS*text/javascript|"

  "GIF*image/gif|"
  "JPG*image/jpeg|"
  "PNG*image/png|"
  "ICO*image/vnd.microsoft.icon|"

  "MP3*audio/mpeg|"
);

void *malloc_check(size_t size) {
  void* r = malloc(size);
#if DEBUG
  if (!r) {
    Serial << F("TWS:No space for malloc: " ); Serial.println(size, DEC);
  }
#endif
  return r;
}

// Offset for text/html in `mime_types' above.
static const TinyWebServer::MimeType text_html_content_type = 4;

TinyWebServer::TinyWebServer(PathHandler handlers[],
			     const char** headers,
                             const int port)
  : handlers_(handlers),
    server_(EthernetServer(port)),
    path_(NULL),
    request_type_(UNKNOWN_REQUEST),
#ifdef __TM4C1294NCPDT__
    client_(EthernetClient(/*255*/)) {//I hope that 255 isn't important
#else
    client_(EthernetClient(255)) {
#endif
  if (headers) {
    int size = 0;
    for (int i = 0; headers[i]; i++) {
      size++;
    }
    headers_ = (HeaderValue*)malloc_check(sizeof(HeaderValue) * (size + 1));
    if (headers_) {
      for (int i = 0; i < size; i++) {
        headers_[i].header = headers[i];
        headers_[i].value = NULL;
      }
      headers_[size].header = NULL;
    }
  }
}

void TinyWebServer::begin() {
  server_.begin();
}

// Process headers.
boolean TinyWebServer::process_headers() {
  if (headers_) {
    // First clear the header values from the previous HTTP request.
    for (int i = 0; headers_[i].header; i++) {
      if (headers_[i].value) {
        free(headers_[i].value);
        // Ensure the pointer is cleared once the memory is freed.
        headers_[i].value = NULL;
      }
    }
  }

  enum State {
    ERROR,
    START_LINE,
    HEADER_NAME,
    HEADER_VALUE,
    HEADER_VALUE_SKIP_INITIAL_SPACES,
    HEADER_IGNORE_VALUE,
    END_HEADERS,
  };
  State state = START_LINE;

  char ch;
  int pos;
  const char* header;
  uint32_t start_time = millis();
  while (1) {
    if (should_stop_processing()) {
      return false;
    }
    if (millis() - start_time > READ_TIMEOUT) {
      return false;
    }
    if (!read_next_char(client_, (uint8_t*)&ch)) {
      continue;
    }
    start_time = millis();
#if DEBUG
    Serial.print(ch);
#endif
    switch (state) {
    case START_LINE:
      if (ch == '\r') {
	break;
      } else if (ch == '\n') {
	state = END_HEADERS;
      } else if (isalnum(ch) || ch == '-') {
	pos = 0;
	buffer[pos++] = ch;
	state = HEADER_NAME;
      } else {
	state = ERROR;
      }
      break;

    case HEADER_NAME:
      if (pos + 1 >= sizeof(buffer)) {
	state = ERROR;
	break;
      }
      if (ch == ':') {
	buffer[pos] = 0;
	header = buffer;
	if (is_requested_header(&header)) {
	  state = HEADER_VALUE_SKIP_INITIAL_SPACES;
	} else {
	  state = HEADER_IGNORE_VALUE;
	}
	pos = 0;
      } else if (isalnum(ch) || ch == '-') {
	buffer[pos++] = ch;
      } else {
	state = ERROR;
	break;
      }
      break;

    case HEADER_VALUE_SKIP_INITIAL_SPACES:
      if (pos + 1 >= sizeof(buffer)) {
	state = ERROR;
	break;
      }
      if (ch != ' ') {
	buffer[pos++] = ch;
	state = HEADER_VALUE;
      }
      break;

    case HEADER_VALUE:
      if (pos + 1 >= sizeof(buffer)) {
	state = ERROR;
	break;
      }
      if (ch == '\n') {
	buffer[pos] = 0;
	if (!assign_header_value(header, buffer)) {
	  state = ERROR;
	  break;
	}
	state = START_LINE;
      } else {
	if (ch != '\r') {
	  buffer[pos++] = ch;
	}
      }
      break;

    case HEADER_IGNORE_VALUE:
      if (ch == '\n') {
	state = START_LINE;
      }
      break;

    default:
      break;
    }

    if (state == END_HEADERS) {
      break;
    }
    if (state == ERROR) {
      return false;
    }
  }
  return true;
}

void TinyWebServer::process() {
  client_ = server_.available();
  if (!client_.connected() || !client_.available()) {
    return;
  }

  boolean is_complete = get_line(buffer, sizeof(buffer));
  if (!buffer[0]) {
    return;
  }
#if DEBUG
  Serial << F("TWS:New request: ");
  Serial.println(buffer);
#endif
  if (!is_complete) {
    // The requested path is too long.
    send_error_code(414);
    client_.stop();
    return;
  }

  char* request_type_str = get_field(buffer, 0);
  request_type_ = UNKNOWN_REQUEST;
  if (!strcmp("GET", request_type_str)) {
    request_type_ = GET;
  } else if (!strcmp("POST", request_type_str)) {
    request_type_ = POST;
  } else if (!strcmp("PUT", request_type_str)) {
    request_type_ = PUT;
  } else if (!strcmp("DELETE", request_type_str)) {
    request_type_ = DELETE;
  }
  
  path_ = get_field(buffer, 1);
  char* question = strchr(buffer, '?');
  if (question != NULL) {
      size_t queryLen = strchr(question, ' ') - question;
      query_ = (char*)malloc(queryLen);
      memcpy(query_, question, queryLen);
  }

  // Process the headers.
  if (!process_headers()) {
    // Malformed header line.
    send_error_code(417);
    client_.stop();
  }
  // Header processing finished. Identify the handler to call.

  boolean should_close = true;
  boolean found = false;
  for (int i = 0; handlers_[i].path; i++) {
    int len = strlen(handlers_[i].path);
#ifndef GLOB_MATCH
    boolean exact_match = !strcmp(path_, handlers_[i].path);
    boolean regex_match = false;
    if (handlers_[i].path[len - 1] == '*') {
      regex_match = !strncmp(path_, handlers_[i].path, len - 1);
    }
    if ((exact_match || regex_match)
#else
    if(amatch(path_, handlers_[i].path)
#endif
	&& (handlers_[i].type == ANY || handlers_[i].type == request_type_)) {
      found = true;
      should_close = (handlers_[i].handler)(*this);
      break;
    }
  }

  if (!found) {
    send_error_code(404);
    // (*this) << F("URL not found: ");
    // client_->print(path_);
    // client_->println();
  }
  if (should_close) {
    client_.stop();
  }

  free(path_);
  if (query_ != NULL) {
      free(query_);
      query_ = NULL;
  }
  free(request_type_str);
}

boolean TinyWebServer::is_requested_header(const char** header) {
  if (!headers_) {
    return false;
  }
  for (int i = 0; headers_[i].header; i++) {
#ifdef __TM4C1294NCPDT__
    if (!strcasecmp(*header, headers_[i].header)) {
#else
    if (!strcmp(*header, headers_[i].header)) {
#endif
      *header = headers_[i].header;
      return true;
    }
  }
  return false;
}

boolean TinyWebServer::assign_header_value(const char* header, char* value) {
  if (!headers_) {
    return false;
  }
  boolean found = false;
  for (int i = 0; headers_[i].header; i++) {
    // Use pointer equality, since `header' must be the pointer
    // inside headers_.
    if (header == headers_[i].header) {
      headers_[i].value = (char*)malloc_check(strlen(value) + 1);
      if (!headers_[i].value) {
	return false;
      }
      strcpy(headers_[i].value, value);
      found = true;
      break;
    }
  }
  return found;
}

FLASH_STRING(content_type_msg, "Content-Type: ");

void TinyWebServer::send_error_code(Client& client, int code) {
#if DEBUG
  Serial << F("TWS:Returning ");
  Serial.println(code, DEC);
#endif
  client << F("HTTP/1.1 ");
  client.print(code, DEC);
  client << F(" OK\r\n");
  if (code != 200) {
    end_headers(client);
  }
}

void TinyWebServer::send_content_type(MimeType mime_type) {
  client_ << content_type_msg;

  char ch;
  int i = mime_type;
  while ((ch = mime_types[i++]) != '|') {
    client_.print(ch);
  }

  client_.println();
}

void TinyWebServer::send_content_type(const char* content_type) {
  client_ << content_type_msg;
  client_.println(content_type);
}

const char* TinyWebServer::get_path() { return path_; }

const TinyWebServer::HttpRequestType TinyWebServer::get_type() {
  return request_type_;
}

const char* TinyWebServer::get_header_value(const char* name) {
  if (!headers_) {
    return NULL;
  }
  for (int i = 0; headers_[i].header; i++) {
  #ifdef __TM4C1294NCPDT__
    if (!strcasecmp(headers_[i].header, name)) {
  #else
  #pragma warning "Headers are case-sensitive!"
    if (!strcmp(headers_[i].header, name)) {
    #endif
      return headers_[i].value;
    }
  }
  return NULL;
}

int parseHexChar(char ch) {
  if (isdigit(ch)) {
    return ch - '0';
  }
  ch = tolower(ch);
  if (ch >= 'a' &&  ch <= 'e') {
    return ch - 'a' + 10;
  }
  return 0;
}

char* TinyWebServer::decode_url_encoded(const char* s) {
  if (!s) {
    return NULL;
  }
  char* r = (char*)malloc_check(strlen(s) + 1);
  if (!r){
    return NULL;
  }
  char* r2 = r;
  const char* p = s;
  while (*s && (p = strchr(s, '%'))) {
    if (p - s) {
      memcpy(r2, s, p - s);
      r2 += (p - s);
    }
    // If the remaining number of characters is less than 3, we cannot
    // have a complete escape sequence. Break early.
    if (strlen(p) < 3) {
      // Move the new beginning to the value of p.
      s = p;
      break;
    }
    uint8_t r = parseHexChar(*(p + 1)) << 4 | parseHexChar(*(p + 2));
    *r2++ = r;
    p += 3;

    // Move the new beginning to the value of p.
    s = p;
  }
  // Copy whatever is left of the string in the result.
  int len = strlen(s);
  if (len > 0) {
    strncpy(r2, s, len);
  }
  // Append the 0 terminator.
  *(r2 + len) = 0;

  return r;
}

char* TinyWebServer::get_file_from_path(const char* path) {
  // Obtain the last path component.
  const char* encoded_fname = strrchr(path, '/');
  if (!encoded_fname) {
    return NULL;
  } else {
    // Skip past the '/'.
    encoded_fname++;
  }
  char* decoded = decode_url_encoded(encoded_fname);
  if (!decoded) {
    return NULL;
  }
  for (char* p = decoded; *p; p++) {
    *p = toupper(*p);
  }
  return decoded;
}

TinyWebServer::MimeType TinyWebServer::get_mime_type_from_filename(
    const char* filename) {
  MimeType r = text_html_content_type;
  if (!filename) {
    return r;
  }

  char* ext = strrchr(filename, '.');
  if (ext) {
    // We found an extension. Skip past the '.'
    ext++;

    char ch;
    int i = 0;
    while (i < mime_types.length()) {
      // Compare the extension.
      char* p = ext;
      ch = mime_types[i];
      while (*p && ch != '*' && toupper(*p) == ch) {
	p++; i++;
	ch = mime_types[i];
      }
      if (!*p && ch == '*') {
	// We reached the end of the extension while checking
	// equality with a MIME type: we have a match. Increment i
	// to reach past the '*' char, and assign it to `mime_type'.
	r = ++i;
	break;
      } else {
	// Skip past the the '|' character indicating the end of a
	// MIME type.
	while (mime_types[i++] != '|')
	  ;
      }
    }
  }
  return r;
}

#ifdef _USE_SD_CARD
void TinyWebServer::send_file(SdFile& file) {
  size_t size;
  while ((size = file.read(buffer, sizeof(buffer))) > 0) {
    if (!client_.connected()) {
      break;
    }
    write((uint8_t*)buffer, size);
  }
}
#endif

size_t TinyWebServer::write(uint8_t c) {
  client_.write(c);
}

size_t TinyWebServer::write(const char *str) {
  client_.write(str);
}

size_t TinyWebServer::write(const uint8_t *buffer, size_t size) {
  client_.write(buffer, size);
}

boolean TinyWebServer::read_next_char(Client& client, uint8_t* ch) {
  if (!client.available()) {
    return false;
  } else {
    *ch = client.read();
    return true;
  }
}

boolean TinyWebServer::get_line(char* buffer, int size) {
  int i = 0;
  char ch;

  buffer[0] = 0;
  for (; i < size - 1; i++) {
    if (!read_next_char(client_, (uint8_t*)&ch)) {
      continue;
    }
    if (ch == '\n') {
      break;
    }
    buffer[i] = ch;
  }
  buffer[i] = 0;
  return i < size - 1;
}

// Returns a newly allocated string containing the field number `which`.
// The first field's index is 0.
// The caller is responsible for freeing the returned value.
char* TinyWebServer::get_field(const char* buffer, int which) {
  char* field = NULL;
  boolean prev_is_space = false;
  int i = 0;
  int field_no = 0;
  int size = strlen(buffer);

  // Locate the field we need. A field is defined as an area of
  // non-space characters delimited by one or more space characters.
  for (; field_no < which; field_no++) {
    // Skip over space characters
    while (i < size && isspace(buffer[i])) {
      i++;
    }
    // Skip over non-space characters.
    while (i < size && !isspace(buffer[i])) {
      i++;
    }
  }

  // Now we identify the end of the field that we want.
  // Skip over space characters.
  while (i < size && isspace(buffer[i])) {
    i++;
  }

  if (field_no == which && i < size) {
    // Now identify where the field ends.
    int j = i;
    while (j < size && !isspace(buffer[j]) && (buffer[j] != '?')) {
      j++;
    }

    field = (char*) malloc_check(j - i + 1);
    if (!field) {
      return NULL;
    }
    memcpy(field, buffer + i, j - i);
    field[j - i] = 0;
  }
  return field;
}

// The PUT handler.

namespace TinyWebPutHandler {

HandlerFn put_handler_fn = NULL;

// Fills in `buffer' by reading up to `num_bytes'.
// Returns the number of characters read.
int read_chars(TinyWebServer& web_server, Client& client,
               uint8_t* buffer, int size) {
  uint8_t ch;
  int pos;
  for (pos = 0; pos < size && web_server.read_next_char(client, &ch); pos++) {
    buffer[pos] = ch;
  }
  return pos;
}

boolean put_handler(TinyWebServer& web_server) {
  web_server.send_error_code(200);
  web_server.end_headers();

  const char* length_str = web_server.get_header_value("Content-Length");
  long length = atol(length_str);
  uint32_t start_time = 0;
  boolean watchdog_start = false;

  EthernetClient client = web_server.get_client();

  if (put_handler_fn) {
    (*put_handler_fn)(web_server, START, NULL, length);
  }

  uint32_t i;
  for (i = 0; i < length && client.connected();) {
    int16_t size = read_chars(web_server, client, (uint8_t*)buffer, 64);
    if (!size) {
      if (watchdog_start) {
        if (millis() - start_time > 30000) {
          // Exit if there has been zero data from connected client
          // for more than 30 seconds.
#if DEBUG
          Serial << F("TWS:There has been no data for >30 Sec.\n");
#endif
          break;
        }
      } else {
        // We have hit an empty buffer, start the watchdog.
        start_time = millis();
        watchdog_start = true;
      }
      continue;
    }
    i += size;
    // Ensure we re-start the watchdog if we get ANY data input.
    watchdog_start = false;

    if (put_handler_fn) {
      (*put_handler_fn)(web_server, WRITE, buffer, size);
    }
  }
  if (put_handler_fn) {
    (*put_handler_fn)(web_server, END, NULL, 0);
  }

  return true;
}

};
const char* getNextKey(const char* text) {
    const char* k = strchr(text, '?');
    if (k == NULL) return strchr(text, '&');
}

size_t TinyWebServer::get_query(const char* key, char* val, size_t maxLen) {
    if (query_ == NULL) return 0;
    const char* keyNext = query_;
    while (keyNext != NULL)
    {
        keyNext = getNextKey(keyNext);
        if (keyNext == NULL) return 0;
        if (strstr(keyNext + 1, key) == keyNext + 1) {
            const char* eq = strchr(keyNext, '=');
            const char* end = getNextKey(eq);
            if (end == NULL) end = eq + strlen(eq);
            size_t len = min(end - eq - 1, maxLen);
            memcpy(val, eq + 1, len);
            return len;
        }
        else {
            keyNext++;
        }
    }
}
#ifdef GLOB_MATCH
/*
 * robust glob pattern matcher
 * ozan s. yigit/dec 1994
 * public domain
 *
 * glob patterns:
 *	*	matches zero or more characters
 *	?	matches any single character
 *	[set]	matches any character in the set
 *	[^set]	matches any character NOT in the set
 *		where a set is a group of characters or ranges. a range
 *		is written as two characters seperated with a hyphen: a-z denotes
 *		all characters between a to z inclusive.
 *	[-set]	set matches a literal hypen and any character in the set
 *	[]set]	matches a literal close bracket and any character in the set
 *
 *	char	matches itself except where char is '*' or '?' or '['
 *	\char	matches char, including any pattern character
 *
 * examples:
 *	a*c		ac abc abbc ...
 *	a?c		acc abc aXc ...
 *	a[a-z]c		aac abc acc ...
 *	a[-a-z]c	a-c aac abc ...
 *
 * $Log: glob.c,v $
 * Revision 1.3  1995/09/14  23:24:23  oz
 * removed boring test/main code.
 *
 * Revision 1.2  94/12/11  10:38:15  oz
 * cset code fixed. it is now robust and interprets all
 * variations of cset [i think] correctly, including [z-a] etc.
 * 
 * Revision 1.1  94/12/08  12:45:23  oz
 * Initial revision
 */

#ifndef NEGATE
#define NEGATE	'^'			/* std cset negation char */
#endif

#define TRUE    1
#define FALSE   0

int
amatch(const char *str, const char *p)
{
	int negate;
	int match;
	int c;

	while (*p) {
		if (!*str && *p != '*')
			return FALSE;

		switch (c = *p++) {

		case '*':
			while (*p == '*')
				p++;

			if (!*p)
				return TRUE;

			if (*p != '?' && *p != '[' && *p != '\\')
				while (*str && *p != *str)
					str++;

			while (*str) {
				if (amatch(str, p))
					return TRUE;
				str++;
			}
			return FALSE;

		case '?':
			if (*str)
				break;
			return FALSE;
/*
 * set specification is inclusive, that is [a-z] is a, z and
 * everything in between. this means [z-a] may be interpreted
 * as a set that contains z, a and nothing in between.
 */
		case '[':
			if (*p != NEGATE)
				negate = FALSE;
			else {
				negate = TRUE;
				p++;
			}

			match = FALSE;

			while (!match && (c = *p++)) {
				if (!*p)
					return FALSE;
				if (*p == '-') {	/* c-c */
					if (!*++p)
						return FALSE;
					if (*p != ']') {
						if (*str == c || *str == *p ||
						    (*str > c && *str < *p))
							match = TRUE;
					}
					else {		/* c-] */
						if (*str >= c)
							match = TRUE;
						break;
					}
				}
				else {			/* cc or c] */
					if (c == *str)
						match = TRUE;
					if (*p != ']') {
						if (*p == *str)
							match = TRUE;
					}
					else
						break;
				}
			}

			if (negate == match)
				return FALSE;
/*
 * if there is a match, skip past the cset and continue on
 */
			while (*p && *p != ']')
				p++;
			if (!*p++)	/* oops! */
				return FALSE;
			break;

		case '\\':
			if (*p)
				c = *p++;
		default:
			if (c != *str)
				return FALSE;
			break;

		}
		str++;
	}

	return !*str;
}

#endif
