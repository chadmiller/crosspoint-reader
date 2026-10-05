#pragma once

#include "ProvisioningEnumParsers.generated.h"

class ProvisioningConfigLoader {
 public:
  // Load provisioning config file, apply settings, and remove applied items from the file.
  // Removes the file entirely if all items are successfully applied.
  // Non-blocking; logs all errors. Returns true if file was processed (or didn't exist).
  // Default filename: /provision.json
  static bool processProvisioningConfig(const char* filename = "/provision.json");

 private:
  // Parse and apply individual sections. Returns true if any items were applied.
  static bool applyWifiSettings(JsonDocument& doc);
  static bool applyOpdsSettings(JsonDocument& doc);

  // Persist modified config back to the provisioning file, or delete if empty.
  // Returns true if successful (file saved or deleted as appropriate).
  static bool persistModifiedConfig(const JsonDocument& doc);

  // Check if the document has any meaningful content left.
  static bool hasRemainingItems(const JsonDocument& doc);

  // Filename being processed; stored during processProvisioningConfig() execution.
  static inline const char* filename = nullptr;
};
