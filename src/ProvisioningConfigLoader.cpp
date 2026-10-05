#include "ProvisioningConfigLoader.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "OpdsServerStore.h"
#include "ProvisioningSchema.generated.h"
#include "WifiCredentialStore.h"

namespace {
constexpr const char* LOG_MODULE = "PROV";

// Apply a single setting from JSON to settings structure
bool applySetting(uint8_t* settingsBase, const JsonVariant& jsonValue, const provisioning::SettingMetadata& setting) {
  if (jsonValue.isNull()) {
    return false;
  }

  uint8_t* memberPtr = settingsBase + setting.memberOffset;

  switch (setting.type) {
    case provisioning::TYPE_BOOL: {
      // JSON bool/number → uint8_t (0 or 1)
      *memberPtr = jsonValue.as<bool>() ? 1 : 0;
      return true;
    }

    case provisioning::TYPE_UINT8: {
      uint8_t val = jsonValue.as<uint8_t>();
      *memberPtr = val;
      return true;
    }

    case provisioning::TYPE_INT8: {
      int8_t val = jsonValue.as<int8_t>();
      *reinterpret_cast<int8_t*>(memberPtr) = val;
      return true;
    }

    case provisioning::TYPE_ENUM: {
      // String → enum value via parser function
      if (!setting.enumParser) {
        LOG_ERR(LOG_MODULE, "No enum parser for %s", setting.memberName);
        return false;
      }

      const char* strValue = jsonValue.as<const char*>();
      if (!strValue) {
        LOG_ERR(LOG_MODULE, "%s: expected string, got %s", setting.memberName,
                jsonValue.is<int>() ? "number" : "other");
        return false;
      }

      int parsed = setting.enumParser(strValue);
      if (parsed < 0) {
        LOG_ERR(LOG_MODULE, "%s: unknown value '%s'", setting.memberName, strValue);
        return false;
      }

      *memberPtr = static_cast<uint8_t>(parsed);
      return true;
    }

    case provisioning::TYPE_STRING: {
      // String → char[] (with bounds checking)
      const char* strValue = jsonValue.as<const char*>();
      if (!strValue) {
        return false;
      }

      // Find the actual size of the char array in the struct
      // For now, assume standard buffer sizes; this could be enhanced with metadata
      size_t bufferSize = 64;  // Default assumption
      if (strcmp(setting.memberName, "sdFontFamilyName") == 0) {
        bufferSize = 32;
      } else if (strcmp(setting.memberName, "dictionaryName") == 0) {
        bufferSize = 32;
      } else if (strcmp(setting.memberName, "opdsDownloadFolder") == 0) {
        bufferSize = 64;
      }

      strncpy(reinterpret_cast<char*>(memberPtr), strValue, bufferSize - 1);
      reinterpret_cast<char*>(memberPtr)[bufferSize - 1] = '\0';
      return true;
    }

    default:
      LOG_ERR(LOG_MODULE, "%s: unknown type %d", setting.memberName, setting.type);
      return false;
  }
}

// Apply all settings from a section (display, text, statusbar, etc.)
bool applySettingsSection(JsonDocument& doc, const char* sectionName, const provisioning::SettingMetadata* settings,
                          size_t settingCount) {
  JsonObject section = doc[sectionName];
  if (section.isNull()) {
    return false;
  }

  bool anyApplied = false;

  // Iterate in reverse to safely remove items during iteration
  for (int i = settingCount - 1; i >= 0; --i) {
    const auto& setting = settings[i];

    if (!section[setting.jsonKey].isNull()) {
      if (applySetting(reinterpret_cast<uint8_t*>(&SETTINGS), section[setting.jsonKey], setting)) {
        LOG_INF(LOG_MODULE, "%s.%s = %s", sectionName, setting.jsonKey, setting.memberName);
        section.remove(setting.jsonKey);
        anyApplied = true;
      } else {
        LOG_ERR(LOG_MODULE, "%s.%s: failed to apply to %s", sectionName, setting.jsonKey, setting.memberName);
      }
    } else {
      LOG_INF(LOG_MODULE, "%s.%s: section was empty", sectionName, setting.jsonKey);
    }
  }

  // Clean up empty section
  if (section.size() == 0) {
    doc.remove(sectionName);
  }

  return anyApplied;
}

}  // namespace

bool ProvisioningConfigLoader::processProvisioningConfig(const char* provJson) {
  if (!provJson) provJson = "/provision.json";

  filename = provJson;

  HalFile file;
  if (!Storage.openFileForRead(LOG_MODULE, filename, file)) {
    LOG_INF(LOG_MODULE, "No provisioning config found at %s", filename);
    return true;  // Not an error; just nothing to do
  }

  LOG_DBG(LOG_MODULE, "Provisioning config found at %s. Size: %u", filename, file.size());

  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    LOG_ERR(LOG_MODULE, "Failed to parse JSON from %s: %s", filename, error.c_str());
    return false;  // Leave file in place for manual fix
  }

  bool anyWifiApplied = applyWifiSettings(doc);
  bool anyOpdsApplied = applyOpdsSettings(doc);

  // Apply schema-driven core settings (as enums are annotated with @schema: markers)
  bool anyCoreApplied = false;
  anyCoreApplied |=
      applySettingsSection(doc, "display", provisioning::displaySettings, provisioning::displaySettingsCount);
  anyCoreApplied |= applySettingsSection(doc, "text", provisioning::textSettings, provisioning::textSettingsCount);
  anyCoreApplied |=
      applySettingsSection(doc, "statusbar", provisioning::statusbarSettings, provisioning::statusbarSettingsCount);
  anyCoreApplied |= applySettingsSection(doc, "ui", provisioning::uiSettings, provisioning::uiSettingsCount);

  // Save modified settings
  if (anyWifiApplied) {
    WIFI_STORE.saveToFile();
  }
  if (anyOpdsApplied) {
    OPDS_STORE.saveToFile();
  }
  if (anyCoreApplied) {
    SETTINGS.saveToFile();
  }

  // Persist or delete the modified config
  return persistModifiedConfig(doc);
}

bool ProvisioningConfigLoader::applyWifiSettings(JsonDocument& doc) {
  JsonObject wifi = doc["wifi"];
  if (wifi.isNull()) {
    return false;  // No WiFi section
  }

  JsonArray networks = wifi["networks"];
  if (networks.isNull()) {
    return false;
  }

  bool anyApplied = false;

  // Iterate in reverse to safely remove items
  for (int i = networks.size() - 1; i >= 0; --i) {
    JsonObject net = networks[i];
    const char* ssid = net["ssid"];
    const char* password = net["password"];

    if (!ssid || !password) {
      LOG_ERR(LOG_MODULE, "WiFi entry %d: missing ssid or password", i);
      continue;  // Leave in file
    }

    if (WIFI_STORE.addCredential(ssid, password)) {
      LOG_INF(LOG_MODULE, "WiFi: Added network '%s'", ssid);
      networks.remove(i);
      anyApplied = true;
    } else {
      LOG_ERR(LOG_MODULE, "WiFi: Failed to add network '%s' (may be duplicate or limit reached)", ssid);
      // Leave in file for retry
    }
  }

  // Clean up empty networks array
  if (networks.size() == 0) {
    wifi.remove("networks");
  } else {
    LOG_INF(LOG_MODULE, "WiFi: Remaining networks: %d", networks.size());
  }

  // Download folder and filename format don't fail, so always remove on presence
  const char* dlFolder = wifi["downloadFolder"];
  if (dlFolder) {
    strncpy(SETTINGS.opdsDownloadFolder, dlFolder, sizeof(SETTINGS.opdsDownloadFolder) - 1);
    SETTINGS.opdsDownloadFolder[sizeof(SETTINGS.opdsDownloadFolder) - 1] = '\0';
    LOG_INF(LOG_MODULE, "OPDS: Download folder = '%s'", dlFolder);
    wifi.remove("downloadFolder");
    anyApplied = true;
  }

  const char* filenameFormat = wifi["filenameFormat"];
  if (filenameFormat) {
    if (strcmp(filenameFormat, "author-title") == 0) {
      SETTINGS.opdsFilenameFormat = 0;
      anyApplied = true;
    } else if (strcmp(filenameFormat, "title-author") == 0) {
      SETTINGS.opdsFilenameFormat = 1;
      anyApplied = true;
    } else if (strcmp(filenameFormat, "title") == 0) {
      SETTINGS.opdsFilenameFormat = 2;
      anyApplied = true;
    } else {
      LOG_ERR(LOG_MODULE, "OPDS: Invalid filenameFormat '%s'", filenameFormat);
      return anyApplied;  // Leave in file
    }
    LOG_INF(LOG_MODULE, "OPDS: Filename format = %d", SETTINGS.opdsFilenameFormat);
    wifi.remove("filenameFormat");
  }

  return anyApplied;
}

bool ProvisioningConfigLoader::applyOpdsSettings(JsonDocument& doc) {
  JsonObject opds = doc["opds"];
  if (opds.isNull()) {
    return false;
  }

  JsonArray servers = opds["servers"];
  if (servers.isNull()) {
    return false;
  }

  bool anyApplied = false;

  // Iterate in reverse to safely remove items
  for (int i = servers.size() - 1; i >= 0; --i) {
    JsonObject srv = servers[i];
    const char* name = srv["name"];
    const char* url = srv["url"];

    if (!name || !url) {
      LOG_ERR(LOG_MODULE, "OPDS server %d: missing name or url", i);
      continue;
    }

    OpdsServer server;
    server.name = name;
    server.url = url;
    server.username = srv["username"] | "";
    server.password = srv["password"] | "";

    if (OPDS_STORE.addServer(server)) {
      LOG_INF(LOG_MODULE, "OPDS: Added server '%s'", name);
      servers.remove(i);
      anyApplied = true;
    } else {
      LOG_ERR(LOG_MODULE, "OPDS: Failed to add server '%s' (may be duplicate or limit reached)", name);
      // Leave in file for retry
    }
  }

  // Clean up empty servers array
  if (servers.size() == 0) {
    opds.remove("servers");
  }

  return anyApplied;
}

bool ProvisioningConfigLoader::persistModifiedConfig(const JsonDocument& doc) {
  if (!hasRemainingItems(doc)) {
    if (Storage.remove(filename)) {
      LOG_INF(LOG_MODULE, "All settings applied; deleted %s", filename);
      return true;
    }
    LOG_ERR(LOG_MODULE, "Failed to delete %s after all settings applied", filename);
    return false;
  }

  // File still has items; save it back for retry on next boot
  HalFile file;
  if (!Storage.openFileForWrite(LOG_MODULE, filename, file)) {
    LOG_ERR(LOG_MODULE, "Failed to open %s for writing", filename);
    return false;
  }

  if (serializeJson(doc, file) == 0) {
    LOG_ERR(LOG_MODULE, "Failed to serialize config to %s", filename);
    return false;
  }

  LOG_INF(LOG_MODULE, "Saved remaining config to %s for retry on next boot", filename);
  return true;
}

bool ProvisioningConfigLoader::hasRemainingItems(const JsonDocument& doc) {
  for (JsonObjectConst::iterator it = doc.as<JsonObjectConst>().begin(); it != doc.as<JsonObjectConst>().end(); ++it) {
    const JsonVariantConst val = it->value();
    if (val.size() > 0) {
      return true;
    }
  }
  return false;
}