#!/usr/bin/env python3
"""
Auto-generate provisioning configuration schema from CrossPointSettings.h.

This script:
1. Parses enum definitions and extracts their @prov(section="...") annotations
2. Parses member declarations with @prov(section="...", json_key="...") annotations
3. Generates string-to-enum conversion functions (kebab-case keys)
4. Generates configuration schema metadata for the loader

Annotation format in CrossPointSettings.h:
  enum EnumName { ... };  // @prov(section="section-name")
  uint8_t memberName = DEFAULT;  // @prov(section="section-name", json_key="customKey")

Example:
  enum ORIENTATION { ... };  // @prov(section="display")
  uint8_t screenInverted = 0;  // @prov(section="display", json_key="nightMode")
  uint8_t fontFamily = NOTOSERIF;  // Uses FONT_FAMILY enum with @prov(section="text")

The generated files are:
  - src/ProvisioningEnumParsers.generated.h
  - src/ProvisioningEnumParsers.generated.cpp
  - src/ProvisioningSchema.generated.h
  - src/ProvisioningSchema.generated.cpp

Usage:
  python3 scripts/gen_provisioning_parsers.py
"""

import re
import os
import sys
from pathlib import Path
from typing import Dict, List, Tuple, Optional, NamedTuple

# Use paths relative to current working directory (works from CLI and PlatformIO)
SETTINGS_H = Path("src") / "CrossPointSettings.h"
OUTPUT_PARSERS_H = Path("src") / "ProvisioningEnumParsers.generated.h"
OUTPUT_PARSERS_CPP = Path("src") / "ProvisioningEnumParsers.generated.cpp"
OUTPUT_SCHEMA_H = Path("src") / "ProvisioningSchema.generated.h"

# Known word splits for camelCase enum names (no underscores)
KNOWN_SPLITS = {
    "NOTOSERIF": ["NOTO", "SERIF"],
    "NOTOSANS": ["NOTO", "SANS"],
    "LEFTALIGN": ["LEFT", "ALIGN"],
    "RIGHTALIGN": ["RIGHT", "ALIGN"],
    "CENTERALIGN": ["CENTER", "ALIGN"],
    "TOPBAR": ["TOP", "BAR"],
    "BOTTOMBAR": ["BOTTOM", "BAR"],
    "COVERMODE": ["COVER", "MODE"],
}

SKIP_ENUMS = {"FONT_FAMILY_COUNT", "STATUS_BAR_PROGRESS_BAR_COUNT", "ORIENTATION_COUNT"}


class SettingMetadata(NamedTuple):
    """Metadata for a provisioning-eligible setting."""
    member_name: str
    json_key: str
    section: str
    member_type: str  # "bool", "uint8_t", "int8_t", "enum", "string"
    enum_type: Optional[str] = None  # e.g., "ORIENTATION" if type is enum
    offset_var: Optional[str] = None  # e.g., "offsetof(CrossPointSettings, screenInverted)"


def parse_enums_from_header() -> Dict[str, List[Tuple[str, str]]]:
    """
    Parse CrossPointSettings.h and extract enum definitions.
    Returns: {enum_name: [(value_name, string_key), ...], ...}
    """
    with open(SETTINGS_H, "r") as f:
        content = f.read()

    enums = {}
    enum_pattern = r"enum\s+(\w+)\s*(?::\s*\w+)?\s*\{([^}]+)\}"

    for match in re.finditer(enum_pattern, content):
        enum_name = match.group(1)
        enum_body = match.group(2)

        if enum_name in SKIP_ENUMS or enum_name.endswith("_COUNT"):
            continue

        values = []
        value_pattern = r"(\w+)\s*=\s*\d+,?(?://\s*([a-z0-9\-]+))?"

        for value_match in re.finditer(value_pattern, enum_body):
            value_name = value_match.group(1)
            comment_override = value_match.group(2)

            if value_name.endswith("_COUNT") or value_name.endswith("_MAX"):
                continue

            if comment_override:
                string_key = comment_override
            else:
                string_key = enum_value_to_string(value_name)

            if string_key:
                values.append((value_name, string_key))

        if values:
            enums[enum_name] = values

    return enums


def parse_enum_schema_markers() -> Dict[str, str]:
    """
    Parse enum type provisioning markers.
    Returns: {enum_name: section_name, ...}

    Format: enum NAME { ... };  // @prov(section="section-name")
           (can also be on the opening line for multi-line enums)
    """
    with open(SETTINGS_H, "r") as f:
        lines = f.readlines()

    enum_schemas = {}

    # Find enum declarations with @prov(section="...") markers
    for i, line in enumerate(lines):
        if "@prov(section=" not in line:
            continue

        # Extract @prov(section="...") annotation
        prov_match = re.search(r'@prov\(section="(\w+)"', line)
        if not prov_match:
            continue

        section_name = prov_match.group(1)

        # Look for enum declaration on this line or nearby
        enum_match = re.search(r"enum\s+(\w+)", line)
        if enum_match:
            enum_name = enum_match.group(1)
            enum_schemas[enum_name] = section_name

    return enum_schemas


def build_enum_value_to_enum_map(enums: Dict[str, List[Tuple[str, str]]],
                                 enum_schemas: Dict[str, str]) -> Dict[str, Tuple[str, Optional[str]]]:
    """
    Build a reverse mapping: enum_value_name → (enum_name, section).

    Returns: {VALUE_NAME: (ENUM_NAME, section), ...}
    where section is None if the enum has no @prov(section="...") marker.

    This lets us identify which enum a member uses by looking at its initializer value.
    E.g., if a member is initialized with CHAPTER_TITLE, we can find that
    CHAPTER_TITLE belongs to STATUS_BAR_TITLE enum which has @prov(section="statusbar").
    """
    value_to_enum = {}

    for enum_name, values in enums.items():
        section = enum_schemas.get(enum_name)  # May be None if enum has no @schema: marker
        for value_name, _ in values:
            value_to_enum[value_name] = (enum_name, section)

    return value_to_enum


def parse_provisioning_settings(enum_schemas: Dict[str, str],
                               value_to_enum: Dict[str, Tuple[str, Optional[str]]]) -> List[SettingMetadata]:
    """
    Parse CrossPointSettings.h and extract provisioning-eligible settings.

    Strategy:
    1. Members using enums with @prov(section="...") markers automatically inherit the section
       (detected by looking up the initializer value in the value→enum mapping)
    2. Members with explicit @prov(section="...") annotations can override or specify their section
    3. For JSON key: auto-derive from member name in kebab-case, override with json_key parameter

    Annotation format:
      enum EnumName { ... };  // @prov(section="section-name")
      uint8_t memberName = VALUE;  // @prov(section="section-name", json_key="customKey")
    """
    with open(SETTINGS_H, "r") as f:
        lines = f.readlines()

    settings = []

    # Process all members, looking for enum uses or @prov(section="...") annotations
    # Track if we're inside a nested struct/class (stop parsing members there)
    in_nested_type = False

    for i, line in enumerate(lines):
        # Detect nested struct/class definitions (members inside these are not direct class members)
        if re.search(r"^\s*(struct|class)\s+\w+\s*{", line):
            in_nested_type = True
            continue

        # Detect end of nested type (closing brace at column 2)
        if in_nested_type and re.search(r"^\s*};", line):
            in_nested_type = False
            continue

        # Skip members inside nested types
        if in_nested_type:
            continue

        # Skip static/const/constexpr declarations
        if re.search(r"\b(static|const|constexpr)\b", line):
            continue

        # Skip ALL_CAPS names (likely constants or COUNT markers)
        if re.search(r"(uint8_t|int8_t|uint16_t|bool|char\s+\w+\[)\s+([A-Z_]+)\s*=", line):
            continue

        # Skip non-member lines
        if not re.search(r"(uint8_t|int8_t|uint16_t|bool|char\s+\w+\[)\s+\w+", line):
            continue

        # Skip lines with just declarations (no initialization)
        if "=" not in line:
            continue

        # Extract member declaration and initializer value
        decl_match = re.search(r"(uint8_t|int8_t|uint16_t|int|bool|char\s+\w+\[)\s+(\w+)\s*=\s*(\w+)", line)
        if not decl_match:
            continue

        member_type_str = decl_match.group(1).strip()
        member_name = decl_match.group(2)
        initializer_value = decl_match.group(3)

        # Normalize type
        if member_type_str == "uint8_t":
            member_type = "uint8_t"
        elif member_type_str == "int8_t":
            member_type = "int8_t"
        elif member_type_str == "uint16_t":
            member_type = "uint16_t"
        elif member_type_str in ("int", "bool"):
            member_type = member_type_str
        elif member_type_str.startswith("char"):
            member_type = "string"
        else:
            continue  # Skip unknown types

        # Try to detect enum type from initializer value using reverse mapping
        enum_type = None
        schema_section = None

        if initializer_value in value_to_enum:
            enum_type, inferred_section = value_to_enum[initializer_value]
            member_type = "enum"
            if inferred_section:  # Only use schema if enum has @schema: marker
                schema_section = inferred_section

        # Check for explicit @prov(section="...", json_key="...") annotation
        prov_match = re.search(r'@prov\(section="(\w+)"(?:,\s*json_key="([^"]+)")?\)', line)
        if prov_match:
            schema_section = prov_match.group(1)  # Override
            json_key_override = prov_match.group(2)
        else:
            json_key_override = None

        # Skip if no schema found (not provisioning-eligible)
        if not schema_section:
            continue

        # Auto-derive JSON key if not overridden
        if json_key_override:
            json_key = json_key_override
        else:
            json_key = member_name_to_json_key(member_name)

        offset_var = f"offsetof(CrossPointSettings, {member_name})"

        settings.append(SettingMetadata(
            member_name=member_name,
            json_key=json_key,
            section=schema_section,
            member_type=member_type,
            enum_type=enum_type,
            offset_var=offset_var
        ))

    return settings


def member_name_to_json_key(member_name: str) -> str:
    """Convert camelCase member name to kebab-case JSON key."""
    # Insert hyphens before uppercase letters (except first)
    result = re.sub(r'(?<!^)(?=[A-Z])', '-', member_name)
    return result.lower()


def enum_value_to_string(value_name: str) -> str:
    """Convert enum value name to kebab-case string key."""
    if value_name in KNOWN_SPLITS:
        parts = KNOWN_SPLITS[value_name]
        return "-".join(p.lower() for p in parts)

    if "_" in value_name:
        return value_name.lower().replace("_", "-")

    if value_name.isupper():
        return value_name.lower()

    result = re.sub(r"([a-z])([A-Z])", r"\1-\2", value_name).lower()
    return result if result else value_name.lower()


def generate_parser_function(enum_name: str, values: List[Tuple[str, str]]) -> Tuple[str, str]:
    """Generate a parser function for a single enum."""
    func_name = f"stringTo{enum_name}"

    body_lines = []
    body_lines.append(f"int {func_name}(const char* str) {{")
    body_lines.append("  if (!str) return -1;")

    for value_name, string_key in values:
        body_lines.append(f'  if (strcmp(str, "{string_key}") == 0) return CrossPointSettings::{value_name};')

    body_lines.append("  return -1;")
    body_lines.append("}")

    decl = f"int {func_name}(const char* str);"

    return decl, "\n".join(body_lines)


def group_settings_by_section(settings: List[SettingMetadata]) -> Dict[str, List[SettingMetadata]]:
    """Group provisioning settings by their section."""
    grouped = {}
    for setting in settings:
        if setting.section not in grouped:
            grouped[setting.section] = []
        grouped[setting.section].append(setting)
    return grouped


def generate_schema_impl(grouped_settings: Dict[str, List[SettingMetadata]],
                         enums: Dict[str, List[Tuple[str, str]]]) -> str:
    """Generate the ProvisioningSchema.generated.cpp implementation."""
    content = """// AUTO-GENERATED FILE - DO NOT CHECK IN
// This file is generated by scripts/gen_provisioning_parsers.py
// It is regenerated on every build from CrossPointSettings.h @prov(section="...") annotations
// Do not manually edit this file; add @prov() comments to CrossPointSettings.h instead

#include "ProvisioningSchema.generated.h"
#include "ProvisioningEnumParsers.generated.h"

#include <cstddef>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"

namespace provisioning {

"""

    # Generate metadata arrays for each section
    for section_name in sorted(grouped_settings.keys()):
        section_settings = grouped_settings[section_name]
        content += f"const SettingMetadata {section_name}Settings[] = {{\n"

        for setting in section_settings:
            # Map setting type to SettingType enum
            if setting.member_type == "bool":
                type_enum = "TYPE_BOOL"
            elif setting.member_type == "uint8_t":
                type_enum = "TYPE_UINT8"
            elif setting.member_type == "int8_t":
                type_enum = "TYPE_INT8"
            elif setting.member_type == "enum":
                type_enum = "TYPE_ENUM"
            elif setting.member_type == "string":
                type_enum = "TYPE_STRING"
            else:
                type_enum = "TYPE_UINT8"  # default

            # Enum parser function pointer
            if setting.enum_type:
                parser = f"provisioning::stringTo{setting.enum_type}"
            else:
                parser = "nullptr"

            content += f'  {{"{setting.json_key}", "{setting.member_name}", {setting.offset_var}, {type_enum}, {parser}}},\n'

        content += f"}};\n"
        content += f"const size_t {section_name}SettingsCount = sizeof({section_name}Settings) / sizeof({section_name}Settings[0]);\n\n"

    content += "}  // namespace provisioning\n"
    content += "\n#pragma GCC diagnostic pop\n"
    return content


def main():
    print(f"Generating provisioning schema from {SETTINGS_H.name}...")

    # Parse enums and their schema markers
    enums = parse_enums_from_header()
    enum_schemas = parse_enum_schema_markers()

    # Build reverse mapping: enum_value → (enum_name, section)
    value_to_enum = build_enum_value_to_enum_map(enums, enum_schemas)

    # Parse provisioning-eligible settings
    settings = parse_provisioning_settings(enum_schemas, value_to_enum)

    if not enums:
        print("ERROR: No enums found in CrossPointSettings.h")
        return 1

    if not settings:
        print("WARNING: No @prov(section=...)-annotated settings found in CrossPointSettings.h")

    print(f"  Found {len(enums)} enums")
    print(f"  Found {len(settings)} provisioning settings")

    # Generate enum parsers
    declarations = []
    implementations = []

    for enum_name in sorted(enums.keys()):
        values = enums[enum_name]
        decl, impl = generate_parser_function(enum_name, values)
        declarations.append(decl)
        implementations.append(impl)

    # Write enum parsers header
    header_content = f"""#pragma once

// AUTO-GENERATED FILE - DO NOT CHECK IN
// This file is generated by scripts/gen_provisioning_parsers.py
// It is regenerated on every build from CrossPointSettings.h enum definitions
// Do not manually edit this file; add inline comments to enum values in CrossPointSettings.h instead

#include "CrossPointSettings.h"

namespace provisioning {{

// String-to-enum conversion functions for provisioning configuration.
// Returns the enum value on success, or -1 if the string is not recognized.
// All strings use kebab-case (lowercase with hyphens), auto-derived from enum value names.
//
// NOTE: To override the auto-generated string key for an enum value, add an inline
// comment with the desired key. E.g.:
//   enum FONT_FAMILY {{
//     NOTOSERIF = 0,   // noto-serif
//     NOTOSANS = 1,    // noto-sans
//   }};

{chr(10).join(declarations)}

}}  // namespace provisioning
"""

    with open(OUTPUT_PARSERS_H, "w") as f:
        f.write(header_content)
    print(f"  ✓ Generated {OUTPUT_PARSERS_H.name}")

    # Write enum parsers implementation
    impl_content = f"""// AUTO-GENERATED FILE - DO NOT CHECK IN
// This file is generated by scripts/gen_provisioning_parsers.py
// It is regenerated on every build from CrossPointSettings.h enum definitions
// Do not manually edit this file; add inline comments to enum values in CrossPointSettings.h instead
// (e.g., VALUE = 0, // custom-string-key)

#include "ProvisioningEnumParsers.generated.h"

#include <cstring>

namespace provisioning {{

{chr(10).join(chr(10) + impl for impl in implementations)}

}}  // namespace provisioning
"""

    with open(OUTPUT_PARSERS_CPP, "w") as f:
        f.write(impl_content)
    print(f"  ✓ Generated {OUTPUT_PARSERS_CPP.name}")

    # Group settings by section
    grouped_settings = group_settings_by_section(settings)

    # Write schema header
    schema_header = """// AUTO-GENERATED FILE - DO NOT CHECK IN
// This file is generated by scripts/gen_provisioning_parsers.py
// It is regenerated on every build from CrossPointSettings.h @prov(section="...") annotations
// Do not manually edit this file; add @prov() comments to CrossPointSettings.h enums and members instead

#pragma once

#include <cstddef>
#include <cstdint>

namespace provisioning {

enum SettingType {
  TYPE_BOOL,
  TYPE_UINT8,
  TYPE_INT8,
  TYPE_ENUM,
  TYPE_STRING,
};

struct SettingMetadata {
  const char* jsonKey;
  const char* memberName;
  size_t memberOffset;
  SettingType type;
  int (*enumParser)(const char*);  // For TYPE_ENUM only
};

"""

    # Generate extern declarations for each section
    for section_name in sorted(grouped_settings.keys()):
        schema_header += f"extern const SettingMetadata {section_name}Settings[];\n"
        schema_header += f"extern const size_t {section_name}SettingsCount;\n\n"

    schema_header += "}  // namespace provisioning\n"

    with open(OUTPUT_SCHEMA_H, "w") as f:
        f.write(schema_header)
    print(f"  ✓ Generated {OUTPUT_SCHEMA_H.name}")

    # Create separate implementation file for schema
    OUTPUT_SCHEMA_CPP = Path("src") / "ProvisioningSchema.generated.cpp"
    schema_impl = generate_schema_impl(grouped_settings, enums)
    with open(OUTPUT_SCHEMA_CPP, "w") as f:
        f.write(schema_impl)
    print(f"  ✓ Generated {OUTPUT_SCHEMA_CPP.name}")

    # Report
    print(f"\nGenerated {len(enums)} enum parser functions:")
    for enum_name, values in sorted(enums.items()):
        print(f"  • {enum_name} ({len(values)} values)")

    print(f"\nGenerated {len(settings)} setting metadata entries ({len(grouped_settings)} sections):")
    for section_name in sorted(grouped_settings.keys()):
        count = len(grouped_settings[section_name])
        print(f"  • {section_name}: {count} settings")

    return 0


if __name__ == "__main__":
    exit(main())
else:
    if "Import" in globals():
        Import("env")
        main()
