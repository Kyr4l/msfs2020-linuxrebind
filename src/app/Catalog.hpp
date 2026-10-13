#pragma once
// The browsable list of aircraft functions (generated from the FlyByWire docs, see tools/build_catalog.py).
#include <string>
#include <vector>

#include "app/Profile.hpp"

namespace lr::app {

struct CatalogEntry {
    std::string panel, section, function, api, name, kind, values, rw, type, remark, description;
    bool hasValue = false;
    double value = 0;
    std::string searchKey;  // lower-cased concatenation for filtering
    Action toAction() const;
};

struct Catalog {
    std::string aircraft;
    std::string sourcePath;
    std::vector<CatalogEntry> entries;
    bool load(const std::string& path, std::string& err);
};

}  // namespace lr::app
