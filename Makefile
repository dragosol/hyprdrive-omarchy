CXX ?= g++

# ponytail: Fedora ships lua.pc, Arch lua5.4.pc
LUA_PC := $(shell pkg-config --exists lua5.4 && echo lua5.4 || echo lua)

# Parallel by default: the edit->build->hyprrestart->gate loop lives and dies
# by this. Override with make -j1 for readable error output.
MAKEFLAGS += -j$(shell nproc)

# -O2 is the shipped artifact and what every pixel gate grades. OPT=-O0 for
# debugging sessions where optimized frames confuse the picture.
OPT ?= -O2

PKGS := pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server \
        xkbcommon libsystemd gio-2.0 gio-unix-2.0 $(LUA_PC)

CXXFLAGS := -fPIC $(OPT) -g -std=c++2b -Wno-narrowing -I.build \
            $(shell pkg-config --cflags $(PKGS))
LDLIBS   := $(shell pkg-config --libs libsystemd gio-2.0 gio-unix-2.0)

ifeq ($(CXX),g++)
    CXXFLAGS += -fno-gnu-unique
endif

SRCS := $(wildcard *.cpp)
OBJS := $(SRCS:%.cpp=.build/%.o)
DEPS := $(OBJS:.o=.d)

VERSION_HEADER = .build/PluginVersion.hpp
VERSION_SCRIPT = scripts/generate-plugin-version.sh

.PHONY: all check clean install uninstall FORCE

# An undefined symbol is a lazy-bind compositor kill (three same-day incidents
# recorded in symcheck.sh's header), so the check IS part of the build. So is
# keycheck: a config key read through the deprecated alias resolves, returns the
# default forever and reports nothing -- see that script's header. And so are
# the host tests: they cost a second, they need no compositor, and a pure
# function that is only ever checked by photographing a tile is checked three
# minutes and one fixture at a time.
all: hyprdrive.so check

hyprdrive.so: $(OBJS)
	$(CXX) -shared $(OBJS) -o $@ $(LDLIBS)

# dock stacks C: the icons are #embed-ded, so their object depends on them.
.build/BuiltinIcons.o: $(wildcard assets/icons/*.svg)

.build/%.o: %.cpp | $(VERSION_HEADER)
	@mkdir -p .build
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

# FORCE re-runs the script every make; the script only touches the header when
# the hash actually changed (cmp -s inside), so incremental builds stay warm.
$(VERSION_HEADER): FORCE $(VERSION_SCRIPT)
	@sh $(VERSION_SCRIPT) $@ .

FORCE:

check: hyprdrive.so
	bash scripts/symcheck.sh
	bash scripts/keycheck.sh
	bash scripts/gen_keys_table.sh --check
	bash scripts/host_tests.sh
	python3 scripts/manifest_check.py
	python3 scripts/private_modes_test.py
	python3 scripts/hdicons_test.py

PREFIX ?= $(HOME)/.local

# check, not just hyprdrive.so: `make install` used to skip symcheck entirely,
# which is how an undefined-symbol .so reached ~/.local/lib and killed every
# login until the plugin was moved aside (2026-09-15).
install: hyprdrive.so check
	install -Dm755 hyprdrive.so $(PREFIX)/lib/hyprdrive/hyprdrive.so
	install -Dm644 assets/scratchpad-symbolic.svg $(PREFIX)/lib/hyprdrive/scratchpad-symbolic.svg
	bash scripts/install-fragments.sh

uninstall:
	bash scripts/uninstall.sh

clean:
	rm -rf .build hyprdrive.so

-include $(DEPS)
