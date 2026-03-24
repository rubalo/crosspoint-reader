#include "KOReaderSyncClient.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Logging.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <ctime>

#include "KOReaderCredentialStore.h"

namespace {
// Device identifier for CrossPoint reader
constexpr char DEVICE_NAME[] = "CrossPoint";
constexpr char DEVICE_ID[] = "crosspoint-reader";

void addAuthHeaders(HTTPClient& http) {
  http.addHeader("Accept", "application/vnd.koreader.v1+json");
  http.addHeader("x-auth-user", KOREADER_STORE.getUsername().c_str());
  http.addHeader("x-auth-key", KOREADER_STORE.getMd5Password().c_str());

  // HTTP Basic Auth (RFC 7617) header. This is needed to support koreader sync server embedded in Calibre Web Automated
  // (https://github.com/crocodilestick/Calibre-Web-Automated/blob/main/cps/progress_syncing/protocols/kosync.py)
  http.setAuthorization(KOREADER_STORE.getUsername().c_str(), KOREADER_STORE.getPassword().c_str());
}

bool isHttpsUrl(const std::string& url) { return url.rfind("https://", 0) == 0; }

// Parse URL into host, port, and path components.
// Preserves the hostname (not resolved to IP) so that HTTPClient sends the
// correct Host header — required for reverse proxies like Traefik/nginx.
bool parseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path) {
  size_t start = url.find("://");
  const bool https = (start != std::string::npos && url.compare(0, 5, "https") == 0);
  start = (start != std::string::npos) ? start + 3 : 0;

  size_t hostEnd = url.find_first_of(":/", start);
  if (hostEnd == std::string::npos) hostEnd = url.size();
  host = url.substr(start, hostEnd - start);
  if (host.empty()) return false;

  port = https ? 443 : 80;
  if (hostEnd < url.size() && url[hostEnd] == ':') {
    size_t portEnd = url.find('/', hostEnd);
    if (portEnd == std::string::npos) portEnd = url.size();
    port = static_cast<uint16_t>(atoi(url.c_str() + hostEnd + 1));
    hostEnd = portEnd;
  }

  path = (hostEnd < url.size()) ? url.substr(hostEnd) : "/";
  return true;
}

// Resolve hostname with retries. Returns true if resolved to a valid (non-zero) IP.
bool resolveHost(const char* hostname, IPAddress& out, int maxRetries = 5) {
  for (int i = 0; i < maxRetries; i++) {
    if (WiFi.hostByName(hostname, out) && out != IPAddress(0, 0, 0, 0)) {
      return true;
    }
    delay(200);
  }
  return false;
}

// Begin an HTTP(S) connection.
// Resolves the hostname ourselves and connects to the IP directly, while passing
// the original hostname to HTTPClient so it sends the correct Host header.
// This is required for reverse proxies (Traefik, nginx) that route based on Host,
// and works around ESP32 DNS resolution quirks.
// plainClient/secureClient must outlive the HTTP request.
bool beginHttp(HTTPClient& http, const std::string& url, WiFiClient& plainClient,
               std::unique_ptr<WiFiClientSecure>& secureClient) {
  std::string host;
  uint16_t port;
  std::string path;
  if (!parseUrl(url, host, port, path)) {
    LOG_ERR("KOSync", "Failed to parse URL: %s", url.c_str());
    return false;
  }

  // Resolve hostname to IP ourselves (with retries)
  IPAddress serverIp;
  if (!resolveHost(host.c_str(), serverIp)) {
    LOG_ERR("KOSync", "DNS resolution failed for: %s", host.c_str());
    return false;
  }
  LOG_DBG("KOSync", "Resolved %s -> %s", host.c_str(), serverIp.toString().c_str());

  http.setReuse(false);

  if (isHttpsUrl(url)) {
    secureClient = std::make_unique<WiFiClientSecure>();
    secureClient->setInsecure();
    // Pre-connect to the resolved IP
    if (!secureClient->connect(serverIp, port)) {
      LOG_ERR("KOSync", "TLS connect failed to %s:%d", serverIp.toString().c_str(), port);
      return false;
    }
    // Pass the connected client with the original hostname (sets Host header correctly)
    http.begin(*secureClient, host.c_str(), port, path.c_str(), true);
  } else {
    // Pre-connect to the resolved IP
    if (!plainClient.connect(serverIp, port)) {
      LOG_ERR("KOSync", "TCP connect failed to %s:%d", serverIp.toString().c_str(), port);
      return false;
    }
    // Pass the connected client with the original hostname (sets Host header correctly)
    http.begin(plainClient, host.c_str(), port, path.c_str());
  }

  return true;
}
}  // namespace

KOReaderSyncClient::Error KOReaderSyncClient::authenticate() {
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/users/auth";
  LOG_DBG("KOSync", "Authenticating: %s", url.c_str());

  HTTPClient http;
  WiFiClient plainClient;
  std::unique_ptr<WiFiClientSecure> secureClient;
  if (!beginHttp(http, url, plainClient, secureClient)) return NETWORK_ERROR;
  addAuthHeaders(http);

  http.setTimeout(5000);
  const int httpCode = http.GET();
  LOG_DBG("KOSync", "Auth response: %d (%s)", httpCode, http.errorToString(httpCode).c_str());
  http.end();

  if (httpCode == 200) {
    return OK;
  } else if (httpCode == 401) {
    return AUTH_FAILED;
  } else if (httpCode < 0) {
    return NETWORK_ERROR;
  }
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::getProgress(const std::string& documentHash,
                                                          KOReaderProgress& outProgress) {
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress/" + documentHash;
  LOG_DBG("KOSync", "Getting progress: %s", url.c_str());

  HTTPClient http;
  WiFiClient plainClient;
  std::unique_ptr<WiFiClientSecure> secureClient;
  if (!beginHttp(http, url, plainClient, secureClient)) return NETWORK_ERROR;
  addAuthHeaders(http);

  const int httpCode = http.GET();

  if (httpCode == 200) {
    // Parse JSON response from response string
    String responseBody = http.getString();
    http.end();

    JsonDocument doc;
    const DeserializationError error = deserializeJson(doc, responseBody);

    if (error) {
      LOG_ERR("KOSync", "JSON parse failed: %s", error.c_str());
      return JSON_ERROR;
    }

    outProgress.document = documentHash;
    outProgress.progress = doc["progress"].as<std::string>();
    outProgress.percentage = doc["percentage"].as<float>();
    outProgress.device = doc["device"].as<std::string>();
    outProgress.deviceId = doc["device_id"].as<std::string>();
    outProgress.timestamp = doc["timestamp"].as<int64_t>();

    LOG_DBG("KOSync", "Got progress: %.2f%% at %s", outProgress.percentage * 100, outProgress.progress.c_str());
    return OK;
  }

  http.end();

  LOG_DBG("KOSync", "Get progress response: %d", httpCode);

  if (httpCode == 401) {
    return AUTH_FAILED;
  } else if (httpCode == 404) {
    return NOT_FOUND;
  } else if (httpCode < 0) {
    return NETWORK_ERROR;
  }
  return SERVER_ERROR;
}

KOReaderSyncClient::Error KOReaderSyncClient::updateProgress(const KOReaderProgress& progress) {
  if (!KOREADER_STORE.hasCredentials()) {
    LOG_DBG("KOSync", "No credentials configured");
    return NO_CREDENTIALS;
  }

  std::string url = KOREADER_STORE.getBaseUrl() + "/syncs/progress";
  LOG_DBG("KOSync", "Updating progress: %s", url.c_str());

  HTTPClient http;
  WiFiClient plainClient;
  std::unique_ptr<WiFiClientSecure> secureClient;
  if (!beginHttp(http, url, plainClient, secureClient)) return NETWORK_ERROR;
  addAuthHeaders(http);
  http.addHeader("Content-Type", "application/json");

  // Build JSON body (timestamp not required per API spec)
  JsonDocument doc;
  doc["document"] = progress.document;
  doc["progress"] = progress.progress;
  doc["percentage"] = progress.percentage;
  doc["device"] = DEVICE_NAME;
  doc["device_id"] = DEVICE_ID;

  std::string body;
  serializeJson(doc, body);

  LOG_DBG("KOSync", "Request body: %s", body.c_str());

  const int httpCode = http.PUT(body.c_str());
  http.end();

  LOG_DBG("KOSync", "Update progress response: %d", httpCode);

  if (httpCode == 200 || httpCode == 202) {
    return OK;
  } else if (httpCode == 401) {
    return AUTH_FAILED;
  } else if (httpCode < 0) {
    return NETWORK_ERROR;
  }
  return SERVER_ERROR;
}

const char* KOReaderSyncClient::errorString(Error error) {
  switch (error) {
    case OK:
      return "Success";
    case NO_CREDENTIALS:
      return "No credentials configured";
    case NETWORK_ERROR:
      return "Network error";
    case AUTH_FAILED:
      return "Authentication failed";
    case SERVER_ERROR:
      return "Server error (try again later)";
    case JSON_ERROR:
      return "JSON parse error";
    case NOT_FOUND:
      return "No progress found";
    default:
      return "Unknown error";
  }
}
