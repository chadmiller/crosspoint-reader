#include "ProvisioningConfigLoader.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include "CrossPointSettings.h"
#include "OpdsServerStore.h"
#include "ProvisioningSchema.generated.h"
#include "SettingsList.h"
#include "WifiCredentialStore.h"

namespace {
constexpr const char* LOG_MODULE = "PROV";

// Apply a single setting from JSON using the SettingsList infrastructure for type-safe validation
// settingsList is passed to avoid repeated expensive calls to getSettingsList()
bool applySetting(const JsonVariant& jsonValue, const provisioning::SettingMetadata& setting,
                  const std::vector<SettingInfo>& settingsList) {
  if (jsonValue.isNull()) {
    return false;
  }

  // Find the corresponding SettingInfo from SettingsList to use its valueSetter
  // This provides type safety, validation, and observability instead of raw offset writes
  auto it = std::find_if(settingsList.begin(), settingsList.end(),
                         [&setting](const SettingInfo& info) { return info.key && strcmp(info.key, setting.jsonKey) == 0; });

  if (it == settingsList.end()) {
    LOG_ERR(LOG_MODULE, "Setting not found in SettingsList: %s", setting.jsonKey);
    return false;
  }

  const SettingInfo& info = *it;

  // Apply based on setting type
  if (setting.type == provisioning::TYPE_ENUM) {
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

    // Use the SettingInfo's valueSetter for type-safe application and validation
    if (info.valueSetter) {
      info.valueSetter(static_cast<uint8_t>(parsed));
      LOG_INF(LOG_MODULE, "%s → %d", setting.jsonKey, parsed);
      return true;
    } else {
      LOG_ERR(LOG_MODULE, "No valueSetter for %s", setting.jsonKey);
      return false;
    }
  }

  // For non-enum types, use the valueSetter with the JSON value
  if (info.valueSetter) {
    uint8_t val = jsonValue.as<uint8_t>();
    info.valueSetter(val);
    LOG_INF(LOG_MODULE, "%s → %d", setting.jsonKey, val);
    return true;
  }

  LOG_ERR(LOG_MODULE, "No valueSetter for %s", setting.jsonKey);
  return false;
}

// Apply all settings from a section (display, text, statusbar, etc.)
bool applySettingsSection(JsonDocument& doc, const char* sectionName, const provisioning::SettingMetadata* settings,
                          size_t settingCount, const std::vector<SettingInfo>& settingsList) {
  JsonObject section = doc[sectionName];
  if (section.isNull()) {
    return false;
  }

  bool anyApplied = false;

  // Iterate in reverse to safely remove items during iteration
  for (int i = settingCount - 1; i >= 0; --i) {
    const auto& setting = settings[i];

    if (!section[setting.jsonKey].isNull()) {
      if (applySetting(section[setting.jsonKey], setting, settingsList)) {
        section.remove(setting.jsonKey);
        anyApplied = true;
      } else {
        LOG_ERR(LOG_MODULE, "%s.%s: failed to apply", sectionName, setting.jsonKey);
      }
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

  // Cache SettingsList to avoid expensive repeated lookups
  const auto settingsList = getSettingsList();

  bool anyWifiApplied = applyWifiSettings(doc);
  bool anyOpdsApplied = applyOpdsSettings(doc);

  // Apply schema-driven core settings (as enums are annotated with @schema: markers)
  bool anyCoreApplied = false;
  anyCoreApplied |=
      applySettingsSection(doc, "display", provisioning::displaySettings, provisioning::displaySettingsCount, settingsList);
  anyCoreApplied |= applySettingsSection(doc, "text", provisioning::textSettings, provisioning::textSettingsCount, settingsList);
  anyCoreApplied |=
      applySettingsSection(doc, "statusbar", provisioning::statusbarSettings, provisioning::statusbarSettingsCount, settingsList);
  anyCoreApplied |= applySettingsSection(doc, "ui", provisioning::uiSettings, provisioning::uiSettingsCount, settingsList);

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
      // Leave in file for manual fix; don't remove
    }
    if (SETTINGS.opdsFilenameFormat <= 2) {
      LOG_INF(LOG_MODULE, "OPDS: Filename format = %d", SETTINGS.opdsFilenameFormat);
      wifi.remove("filenameFormat");
    }
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
  } else {
    LOG_INF(LOG_MODULE, "OPDS: Remaining servers: %d", servers.size());
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
  // Check if there are any non-empty top-level sections remaining
  for (JsonObjectConst::iterator it = doc.as<JsonObjectConst>().begin(); it != doc.as<JsonObjectConst>().end(); ++it) {
    const JsonVariantConst val = it->value();
    // For containers (arrays/objects): check if they have items
    // For scalars (string/number/bool): they're not expected at top level, but treat as remaining
    if (!val.isNull() && (val.size() > 0 || (!val.is<JsonObjectConst>() && !val.is<JsonArrayConst>()))) {
      return true;
    }
  }
  return false;
}