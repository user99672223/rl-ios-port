# Builds bc_textures.exe, a 64-bit Windows program, with mingw-w64 (Linux: x86_64-w64-mingw32-g++,
# CI on macOS: llvm-mingw's x86_64-w64-mingw32-clang++). vectors.h / shaders.h are checked in;
# `make regen` rebuilds them from the generators.
ifeq ($(origin CXX),default)
CXX := x86_64-w64-mingw32-g++
endif
CXXFLAGS ?= -O1 -g0 -std=c++17 -Wall -Wextra -Wno-unused-parameter
LDFLAGS ?= -static -static-libgcc -static-libstdc++

bc_textures.exe: bc_textures.cpp vectors.h shaders.h
	$(CXX) $(CXXFLAGS) bc_textures.cpp -o $@ $(LDFLAGS) -ld3d11 -ldxgi

regen:
	python3 gen_vectors.py > vectors.h
	python3 gen_shaders.py > shaders.h

clean:
	rm -f bc_textures.exe

.PHONY: regen clean
