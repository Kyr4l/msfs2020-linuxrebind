#include "app/Catalog.hpp"

#include <algorithm>
#include <cctype>
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
        a.kind = ActionKind::LVarSet;
        a.name = name;
        a.value = hasValue ? value : 1;
    } else if (kind == "avar") {
        a.kind = ActionKind::CalcCode;
        a.name = "1 (>A:" + name + ", Bool)";
    } else {
        a.kind = ActionKind::SimEvent;
        a.name = name;
        a.value = hasValue ? value : 0;
    }
    return a;
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
