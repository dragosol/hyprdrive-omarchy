CXX ?= g++

# The Lua Hyprland itself is linked to (scripts/lua_pc.sh): its headers' constants
# must match the liblua the compositor runs. Arch has `lua` 5.5 (Hyprland's) AND
# `lua54` (via libinput); the old "lua5.4 whenever it exists" pick crashed
# `hyprctl hyprdrive-binds` (dragosol/Hyprdrive#1).
LUA_PC := $(shell sh scripts/lua_pc.sh)

# Parallel by default: the edit->build->hyprrestart->gate loop lives and dies
# by this. Override with make -j1 for readable error output.
MAKEFLAGS += -j$(shell nproc)

# -O2 is the shipped artifact and what every pixel gate grades. OPT=-O0 for
# debugging sessions where optimized frames confuse the picture.
OPT ?= -O2

PKGS := pixman-1 libdrm hyprland pangocairo libinput libudev wayland-server \
        xkbcommon libsystemd gio-2.0 gio-unix-2.0 $(LUA_PC)

# The sources live in src/<area>/ (one folder per subsystem; see src/README.md).
# Every #include is still a bare name: each folder is on the include path, and no
# two files share a name, so the objects stay flat in .build/. --embed-dir=. is
# for BuiltinIcons.cpp's #embed "assets/icons/...", which resolves from the root.
SRC_DIRS := $(sort $(dir $(wildcard src/*/*.cpp src/*/*.hpp)))
SRC_INC  := $(addprefix -I,$(SRC_DIRS:/=))
SRCS := $(wildcard src/*/*.cpp)
OBJS := $(addprefix .build/,$(notdir $(SRCS:.cpp=.o)))
DEPS := $(OBJS:.o=.d)
vpath %.cpp $(SRC_DIRS)
ifneq ($(words $(SRCS)),$(words $(sort $(notdir $(SRCS)))))
$(error two source files share a name: $(filter-out $(sort $(notdir $(SRCS))),$(notdir $(SRCS))) -- objects are flat in .build/)
endif

CXXFLAGS := -fPIC $(OPT) -g -std=c++2b -Wno-narrowing -I.build $(SRC_INC) --embed-dir=. \
            $(shell pkg-config --cflags $(PKGS))
LDLIBS   := $(shell pkg-config --libs libsystemd gio-2.0 gio-unix-2.0 xcb)

ifeq ($(CXX),g++)
    CXXFLAGS += -fno-gnu-unique
endif

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
	bash tests/host_tests.sh
	python3 tests/manifest_check.py
	python3 tests/schema_keys_test.py
	python3 tests/config_defaults_test.py
	python3 tests/private_modes_test.py
	python3 tests/hdicons_test.py
	python3 tests/update_cli_test.py
	python3 tests/fragment_test.py
	python3 tests/shell_restart_test.py
	python3 tests/keybind_editor_test.py
	python3 tests/agent_run_test.py
	python3 tests/tool_scripts_test.py

# ONE PREFIX ON OMARCHY (review, 2026-11-09): the config fragments, the loader
# fragment and uninstall all look in ~/.local, so a PREFIX= here moved only the
# .so and agent-run -- the agent launch then failed, and uninstall left them.
# The native build supports PREFIX end to end; this one installs to ~/.local.
ifeq ($(origin PREFIX),command line)
$(warning PREFIX is ignored on Omarchy: Hyprdrive installs to ~/.local, where its config and uninstall look)
endif
override PREFIX := $(HOME)/.local

# check, not just hyprdrive.so: `make install` used to skip symcheck entirely,
# which is how an undefined-symbol .so reached ~/.local/lib and killed every
# login until the plugin was moved aside (2026-09-15).
install: hyprdrive.so check
	install -Dm755 hyprdrive.so $(PREFIX)/lib/hyprdrive/hyprdrive.so
ifeq ($(KEEP_DEBUG),)
	strip --strip-debug $(PREFIX)/lib/hyprdrive/hyprdrive.so
endif
	install -Dm644 assets/scratchpad-symbolic.svg $(PREFIX)/lib/hyprdrive/scratchpad-symbolic.svg
	install -Dm755 ui/bin/agent-run $(PREFIX)/lib/hyprdrive/agent-run
	bash scripts/install-fragments.sh

uninstall:
	bash scripts/uninstall.sh

clean:
	rm -rf .build hyprdrive.so

# 1.9.50 moved the sources into src/, and a .build/ left by an older release holds
# .d files that name ./AppInfo.cpp and the rest. Reading them stopped every update
# across 1.9.50 at "No rule to make target 'AppInfo.cpp'" until a make clean
# (2026-11-09). An empty rule for each old name lets make rebuild those objects
# from src/ instead, the way -MP already treats a header that is gone.
$(notdir $(SRCS)):
$(addprefix src/,$(notdir $(wildcard src/*/*.cpp src/*/*.hpp))):

-include $(DEPS)
