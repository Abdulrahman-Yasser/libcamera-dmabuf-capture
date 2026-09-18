#include "bev_setup.h"

#include <cmath>

mat3 measured_H(const mat3 &Hb2i, int img_w, int img_h,
                double px_per_m, int bev_w, int bev_h,
                double dx, double dy, double dyaw_deg) {
    (void)img_w; (void)img_h; // kept for call-site compatibility; see CAL_W/CAL_H
    const double r = px_per_m;
    const double Xc = 0.27, Yc = 0.36;
    mat3 S;
    S.at(0,0)= 1.0/r; S.at(0,1)= 0.0;   S.at(0,2)= -(bev_w/2.0)/r + Xc;
    S.at(1,0)= 0.0;   S.at(1,1)=-1.0/r; S.at(1,2)=  (bev_h/2.0)/r + Yc;
    S.at(2,0)= 0.0;   S.at(2,1)= 0.0;   S.at(2,2)= 1.0;

    // Rigid transform in board-metre space: rotate by dyaw about the board
    // origin, then translate by (dx,dy). Identity when untouched, so an
    // un-tuned slot renders exactly as before this fix.
    const double rad = dyaw_deg * M_PI / 180.0;
    const double ca = std::cos(rad), sa = std::sin(rad);
    mat3 T;
    T.at(0,0)= ca; T.at(0,1)=-sa; T.at(0,2)= dx;
    T.at(1,0)= sa; T.at(1,1)= ca; T.at(1,2)= dy;
    T.at(2,0)= 0;  T.at(2,1)=  0; T.at(2,2)= 1;

    mat3 H = Hb2i * T * S;
    for (int col = 0; col < 3; ++col) { H.at(0,col) /= CAL_W; H.at(1,col) /= CAL_H; }
    return H;
}

std::string lens_calib_path(const std::string &lens_dir, const std::string &lens_model)
{
    if (lens_dir.empty() || lens_dir.back() == '/') return lens_dir + lens_model + "-lens.ini";
    return lens_dir + "/" + lens_model + "-lens.ini";
}
