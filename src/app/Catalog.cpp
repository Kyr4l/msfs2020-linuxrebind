#include "app/Catalog.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

namespace lr::app {

static std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

Action CatalogEntry::toAction() const {
    Action a;
    if (kind == "h_event") {
        a.kind = ActionKind::HEvent;
        a.name = name;
    } else if (kind == "lvar") {
        a.name = name;
        if (values == "0|1" && !hasValue) {
            a.kind = ActionKind::LVarToggle;  // push buttons: flip the state each press
        } else {
            a.kind = ActionKind::LVarSet;
            a.value = hasValue ? value : 1;
        }
    } else if (kind == "avar") {
        a.kind = ActionKind::CalcCode;
        a.name = "1 (>A:" + name + ", Bool)";
    } else if (kind == "calc_code") {
        a.kind = ActionKind::CalcCode;
        a.name = name;
    } else {
        a.kind = ActionKind::SimEvent;
        a.name = name;
        a.value = hasValue ? value : 0;
    }
    return a;
}

std::vector<Action> CatalogEntry::toActions() const {
    if (codes.size() <= 1) return {toAction()};
    std::vector<Action> out;
    for (const auto& c : codes) {
        Action a;
        a.kind = ActionKind::CalcCode;
        a.name = c;
        out.push_back(a);
    }
    return out;
}

bool CatalogEntry::isCaptainSide() const {
    return api.find("_L_") != std::string::npos || api.find("EFIS_L_") != std::string::npos;
}

std::string CatalogEntry::firstOfficerApi() const {
    std::string r = api;
    size_t pos = r.find("EFIS_L_");
    if (pos != std::string::npos) { r[pos + 5] = 'R'; return r; }
    pos = r.find("_L_");
    if (pos != std::string::npos) { r[pos + 1] = 'R'; return r; }
    return {};
}

bool CatalogEntry::isIncDec() const {
    for (const char* suffix : {"_INC", "_DEC", "_INCREMENT", "_DECREMENT"})
        if (api.size() >= strlen(suffix) && api.compare(api.size() - strlen(suffix), strlen(suffix), suffix) == 0) return true;
    return api.find("BARO_INC") != std::string::npos || api.find("BARO_DEC") != std::string::npos;
}

bool Catalog::load(const std::string& path, std::string& err) {
    std::ifstream in(path);
    if (!in) { err = "cannot open " + path; return false; }
    try {
        nlohmann::json j;
        in >> j;
        aircraft = j.value("aircraft", "");
        entries.clear();
        for (const auto& e : j.value("entries", nlohmann::json::array())) {
            CatalogEntry c;
            c.panel = e.value("panel", "");
            c.section = e.value("section", "");
            c.function = e.value("function", "");
            c.api = e.value("api", "");
            c.name = e.value("name", c.api);
            c.kind = e.value("kind", "sim_event");
            c.values = e.value("values", "");
            c.rw = e.value("rw", "");
            c.type = e.value("type", "");
            c.remark = e.value("remark", "");
            c.description = e.value("description", "");
            if (e.contains("value") && e["value"].is_number()) { c.hasValue = true; c.value = e["value"].get<double>(); }
            for (const auto& code : e.value("codes", nlohmann::json::array())) c.codes.push_back(code.get<std::string>());
            c.searchKey = lower(c.panel + " " + c.section + " " + c.function + " " + c.api + " " + c.type + " " + c.remark + " " + c.description);
            entries.push_back(std::move(c));
        }
        sourcePath = path;
    } catch (const std::exception& e) {
        err = std::string("parse error: ") + e.what();
        return false;
    }
    return true;
}

}  // namespace lr::app
