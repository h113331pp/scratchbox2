
# Not needed anymore, because Lua was dropped from libsb2:
#	$(D)/paths_luaif.o $(D)/pathresolution_lua_engine.o

objs := $(D)/pathresolution_c_engine.o \
	$(D)/pathlistutils.o $(D)/pathmapping_interf.o \
	$(D)/paths_ruletree_mapping.o \
	$(D)/paths_ruletree_maint.o

pathmapping/libpaths.a: $(objs)
pathmapping/libpaths.a: override CFLAGS := $(CFLAGS) -O2 -g -fPIC -Wall -W -I$(SRCDIR)/$(LUASRC) -I$(OBJDIR)/preload -I$(SRCDIR)/preload -I$(SRCDIR)/pathmapping \
	-Werror -Wmissing-prototypes -Wstrict-prototypes
pathmapping/libpaths.a: override LDFLAGS := $(LDFLAGS)
pathmapping/libpaths.a: override LIBS := 

targets := $(targets) $(D)/libpaths.a

