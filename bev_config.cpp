#include "bev_config.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

BevConfig bev_config_defaults()
{
    BevConfig cfg;
    // Slots 0/1 are the two real ChArUco-calibrated test cameras: both
    // mounted 45cm up, 19cm apart (9.5cm either side of center), facing
    // forward -- slot 0 (camA) on the left yawed a little left, slot 1
    // (camB) on the right yawed a little right (toed-out) -- so both get
    // facing_deg=90 (front), not the 90-degree-apart front/right split this
    // template originally assumed. Slots 2/3 are still-unused placeholders
    // for a future back/left camera pair.
    // Recalibrated from captures/20260623_100123's camA.png/camB.png (a
    // single shared ChArUco board shot, both cameras, not moved between
    // frames) via cv2.findHomography(board-metres -> image-pixels) per
    // camera, RANSAC, then rescaled from that capture's 1640x1232 into the
    // 3280x2464 CAL_W/CAL_H space main.cpp's measured_H() assumes (see its
    // comment -- must match the resolution the board was shot at, not the
    // runtime video's own size). 21 shared corners: median=0.77mm,
    // mean=0.86mm, max=2.42mm overlap alignment error -- replaces an
    // earlier pair of hb2i values that, while each individually plausible,
    // didn't share a common board position with each other and produced a
    // large, visible camA/camB mismatch in the rendered overlap band.
    cfg.slots[0] = BevSlotConfig{
        -0.095, 0.0, 0.45, -30.0, 15.0, 90.0, true,
        mat3{{ -3081.986, -135.1398, -0.1623139, 1576.966, 657.943, 1.077301, 1729.896, 1869.204, 1 }}
    };
    cfg.slots[1] = BevSlotConfig{
        0.095, 0.0, 0.45, -30.0, -15.0, 90.0, true,
        mat3{{ -2520.185, 298.7253, 0.3775643, 2816.847, 798.551, 1.133288, 3326.267, 2178.929, 1 }}
    };
    cfg.slots[2] = BevSlotConfig{ 0.0, -1.0, 1.2, -30.0, 180.0, 270.0, false, mat3{} };
    cfg.slots[3] = BevSlotConfig{ -0.5, 0.0, 1.2, -30.0,  90.0, 180.0, false, mat3{} };
    return cfg;
}

// Accepts a cardinal keyword (front/back/left/right, case-insensitive) or a
// raw degree number -- same bearing convention as BevSlotConfig::facing_deg.
static bool parse_facing(const std::string &val, double &out)
{
    std::string low = val;
    for (char &c : low) c = (char)std::tolower((unsigned char)c);
    if      (low == "front") { out = 90.0;  return true; }
    else if (low == "right") { out = 0.0;   return true; }
    else if (low == "back")  { out = 270.0; return true; }
    else if (low == "left")  { out = 180.0; return true; }
    try {
        size_t consumed = 0;
        out = std::stod(val, &consumed);
        return consumed == val.size();
    } catch (...) {
        return false;
    }
}

static std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Parses a "[slotN]" section header; returns false if malformed.
static bool parse_slot_header(const std::string &line, int &idx)
{
    if (line.size() < 3 || line.front() != '[' || line.back() != ']') return false;
    std::string inner = line.substr(1, line.size() - 2);
    if (inner.rfind("slot", 0) != 0) return false;
    std::string num = inner.substr(4);
    if (num.empty()) return false;
    try {
        size_t consumed = 0;
        idx = std::stoi(num, &consumed);
        return consumed == num.size();
    } catch (...) {
        return false;
    }
}

bool bev_config_load(const std::string &path, BevConfig &out)
{
    out = bev_config_defaults();

    std::ifstream in(path);
    if (!in.is_open()) {
        std::printf("[config] '%s' not found, using built-in defaults\n", path.c_str());
        return true;
    }

    int current_slot = -1; // -1 = global section
    std::string raw;
    int lineno = 0;
    while (std::getline(in, raw)) {
        ++lineno;
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;

        if (line.front() == '[') {
            int idx;
            if (!parse_slot_header(line, idx) || idx < 0 || idx >= BevConfig::kMaxSlots) {
                std::cerr << "[config] " << path << ":" << lineno
                          << ": bad section '" << line << "'\n";
                return false;
            }
            current_slot = idx;
            continue;
        }

        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            std::cerr << "[config] " << path << ":" << lineno
                      << ": expected 'key = value', got '" << line << "'\n";
            return false;
        }
        std::string key = trim(line.substr(0, eq));
        std::string val = trim(line.substr(eq + 1));

        try {
            if (current_slot < 0) {
                if      (key == "px_per_m")    out.px_per_m    = std::stod(val);
                else if (key == "overlap_deg") out.overlap_deg = std::stof(val);
                else if (key == "blend_edge")  out.blend_edge  = std::stof(val);
                else {
                    std::cerr << "[config] " << path << ":" << lineno
                              << ": unknown global key '" << key << "'\n";
                    return false;
                }
            } else {
                BevSlotConfig &s = out.slots[current_slot];
                if      (key == "cam_x") s.cam_x = std::stod(val);
                else if (key == "cam_y") s.cam_y = std::stod(val);
                else if (key == "cam_h") s.cam_h = std::stod(val);
                else if (key == "pitch") s.pitch = std::stod(val);
                else if (key == "yaw")   s.yaw   = std::stod(val);
                else if (key == "facing") {
                    if (!parse_facing(val, s.facing_deg)) {
                        std::cerr << "[config] " << path << ":" << lineno
                                  << ": bad facing '" << val
                                  << "' (want front/back/left/right or a degree number)\n";
                        return false;
                    }
                }
                else if (key == "hb2i") {
                    std::istringstream iss(val);
                    double v[9];
                    for (double &d : v) {
                        if (!(iss >> d)) {
                            std::cerr << "[config] " << path << ":" << lineno
                                      << ": hb2i needs 9 values, got '" << val << "'\n";
                            return false;
                        }
                    }
                    for (int c = 0; c < 9; ++c) s.hb2i.m[c] = v[c];
                    s.has_hb2i = true;
                } else {
                    std::cerr << "[config] " << path << ":" << lineno
                              << ": unknown slot key '" << key << "'\n";
                    return false;
                }
            }
        } catch (const std::exception &) {
            std::cerr << "[config] " << path << ":" << lineno
                      << ": malformed number in '" << line << "'\n";
            return false;
        }
    }
    return true;
}

bool bev_config_save(const std::string &path, const BevConfig &cfg)
{
    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "[config] failed to open '" << path << "' for writing\n";
        return false;
    }

    out << std::setprecision(9);
    out << "# libcamera-dmabuf-capture -- multi-camera (--src) BEV defaults.\n"
        << "# Auto-generated; press 'W' at runtime to overwrite with live-tuned values.\n\n";
    out << "px_per_m = "    << cfg.px_per_m    << "\n";
    out << "overlap_deg = " << cfg.overlap_deg << "\n";
    out << "blend_edge = "  << cfg.blend_edge  << "\n";

    for (int i = 0; i < BevConfig::kMaxSlots; ++i) {
        const BevSlotConfig &s = cfg.slots[i];
        out << "\n[slot" << i << "]\n";
        out << "cam_x = " << s.cam_x << "\n";
        out << "cam_y = " << s.cam_y << "\n";
        out << "cam_h = " << s.cam_h << "\n";
        out << "pitch = " << s.pitch << "\n";
        out << "yaw = "   << s.yaw   << "\n";
        out << "facing = " << s.facing_deg << "\n";
        if (s.has_hb2i) {
            out << "hb2i =";
            for (double v : s.hb2i.m) out << " " << v;
            out << "\n";
        }
    }
    return (bool)out;
}
