#pragma once
#include "Arduino.h"
#define HTTP_GET 1
#define HTTP_POST 2
#define CONTENT_LENGTH_UNKNOWN ((size_t)-1)
/* The upload path, reproduced closely enough that the real signatures are what
   gets compiled. A frame map arrives as multipart/form-data and is streamed to
   the card a chunk at a time, so the handler is called repeatedly with a status
   rather than once with a body - which is the part worth type-checking here. */
enum HTTPUploadStatus {
  UPLOAD_FILE_START = 0,
  UPLOAD_FILE_WRITE,
  UPLOAD_FILE_END,
  UPLOAD_FILE_ABORTED
};
struct HTTPUpload {
  HTTPUploadStatus status = UPLOAD_FILE_START;
  String   filename;
  size_t   currentSize = 0;
  size_t   totalSize   = 0;
  uint8_t  buf[1];
};

class WebServer {
public:
  WebServer(uint16_t){}
  void on(const char*, int, void(*)()) {}
  void on(const char*, int, void(*)(), void(*)()) {}
  HTTPUpload &upload() { static HTTPUpload u; return u; }
  void onNotFound(void(*)()) {}
  void begin() {}
  void handleClient() {}
  void send(int, const char*, const String&) {}
  void send(int, const char*, const char*) {}
  void send_P(int, const char*, const char*) {}
  void setContentLength(size_t) {}
  void sendContent(const String&) {}
  void sendContent(const char*) {}
  void sendContent_P(const char*) {}
  void sendContent_P(const char*, size_t) {}
  void sendHeader(const char*, const char*) {}
  bool hasArg(const char*) { return false; }
  String arg(const char*) { return String("0"); }
  int args() { return 0; }
};
