#include <fmt/format.h>
#include "events.h"
#include "rapidjson/writer.h"

using namespace uWS;
using namespace rapidjson;
using namespace std;

// Sends an event to the client with a given JSON document
void sendEvent(WebSocket<SERVER> *ws, Document &document) {
  StringBuffer buffer;
  Writer<StringBuffer> writer(buffer);
  document.Accept(writer);
  string jsonPayload = buffer.GetString();
  ws->send(jsonPayload.c_str(), jsonPayload.size(), uWS::TEXT);
}

// Sends an event to the client with a given JSON document, prepended by a binary payload of the specified length
void sendEventBinaryPayload(WebSocket<SERVER> *ws, Document &document, void *payload, uint32_t length) {
  StringBuffer buffer;
  Writer<StringBuffer> writer(buffer);
  document.Accept(writer);
  string jsonPayload = buffer.GetString();
  auto rawData = new char[jsonPayload.size() + length + sizeof(length)];
  memcpy(rawData, &length, sizeof(length));
  memcpy(rawData + sizeof(length), payload, length);
  memcpy(rawData + length + sizeof(length), jsonPayload.c_str(), jsonPayload.size());
  ws->send(rawData, jsonPayload.size() + length + sizeof(length), uWS::BINARY);
  delete[] rawData;
}

// Sends an event to the client with a given event name (padded/concatentated to 32 characters) and a given protobuf message
void sendEvent(uWS::WebSocket<uWS::SERVER> *ws, string eventName, void *protocolPayload, int length) {
  size_t eventNameLength = 32;
  auto rawData = new char[eventNameLength + length];
  memset(rawData, 0, eventNameLength);
  memcpy(rawData, eventName.c_str(), min(eventName.length(), eventNameLength));
  memcpy(rawData+eventNameLength, protocolPayload, length);
  ws->send(rawData, eventNameLength + length, uWS::BINARY);
  delete[] rawData;
}
