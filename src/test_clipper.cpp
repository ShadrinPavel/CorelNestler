#include "clipper2/clipper.h"
#include <iostream>
using namespace Clipper2Lib;
int main() {
    Paths64 subj, clip, sol;
    subj.push_back(MakePath({10,10, 110,10, 110,110, 10,110}));
    clip.push_back(MakePath({50,50, 150,50, 150,150, 50,150}));
    sol = Intersect(subj, clip, FillRule::NonZero);
    if(sol.size() > 0) std::cout << "Clipper2 OK." << std::endl;
    return 0;
}