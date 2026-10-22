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
    std::vector<std::string> codes;  // recipes with several RPN snippets (one action each)
    std::string searchKey;  // lower-cased concatenation for filtering
    Action toAction() const;
    std::vector<Action> toActions() const;  // all actions of the entry (recipes may have several)
    bool isCaptainSide() const;            // api names the L (captain) side, e.g. A32NX.FCU_EFIS_L_BARO_INC
    std::string firstOfficerApi() const;   // same api with the side swapped to R
    bool isIncDec() const;                 // +/- style entry, best bound with repeat-while-held
};

struct Catalog {
    std::string aircraft;   // id, e.g. "A32NX"
    std::string title;      // display name
    std::string label() const { return title.empty() ? aircraft : title; }
    std::string sourcePath;
    std::vector<CatalogEntry> entries;
    bool load(const std::string& path, std::string& err);
};

}  // namespace lr::app
