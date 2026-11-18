cmake_minimum_required(VERSION 3.15)
project(FxTxtViewer C)
include(GenerateG1A)
find_package(Gint 2.9 REQUIRED)
add_executable(fxtxt src/main.c src/font_ascii.c src/font_cjk8.c src/gb_map.c src/gb_rev.c src/pinyin.c)
target_compile_options(fxtxt PRIVATE -Wall -Wextra -Os -ffunction-sections -fdata-sections)
target_link_options(fxtxt PRIVATE -Wl,--gc-sections)
target_link_libraries(fxtxt Gint::Gint)
generate_g1a(TARGET fxtxt OUTPUT "FXTXT.G1A" NAME "FxText" INTERNAL "@FXTXT" VERSION "01.00.0000" ICON "assets/icon.png")



