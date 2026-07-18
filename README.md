# LICENSE #
This software is open source software licensed under GPL3, as found in
the COPYING file.

# BUILDING #
This project requires CMake to be built. 

It also requires the
following libraries:

* OpenGL
* StormLib (by Ladislav Zezula)
* CascLib (by Ladislav Zezula)
* Qt5
* Lua5.x

On Windows you only need to install Qt5 yourself, the rest of the dependencies are pulled through FetchContent automatically.
Supporting for Linux and Mac for this feature is coming in the future.
In case FetchContent is not available (e.g. no internet connection), the find scripts will look for system installed libraries.

Further following libraries are required for MySQL GUID Storage builds:

* LibMySQL
* MySQLCPPConn
See below for detailed instructions

## Docker (Linux) ##

Build Noggit, run its tests in Docker, then export the executable and its
missing runtime libraries into the ignored `out/` directory:

```bash
./scripts/build-cpp-docker.sh
```

Only Docker and the exported files are written on the host. To launch Noggit:

```bash
(cd out && LD_LIBRARY_PATH="$PWD/lib" ./noggit)
```

Pour tester le générateur MOBA sans appeler OpenAI, ouvre une carte carrée
complète de 2×2 à 4×4 tuiles puis clique sur **Lab blueprint MOBA** dans le
dock assistant. Le JSON prérempli peut être compilé sans modifier la carte ;
**Compiler et exécuter** lance ensuite exactement terrain, eau, végétation et
validation. La dernière spécification utilisée est conservée dans les réglages.

Open a map, then use **Assist > AI Assistant** and paste a fresh key into the
masked API key field. The key stays in process memory and is not saved;
`OPENAI_API_KEY` remains available as a fallback. A read-only smoke test is:
`Recherche 10 textures contenant grass.` The API key is never copied into the
image. Remove the generated files with `rm -rf out`, and Docker build artifacts
with `docker image rm noggit-purple:local` and `docker builder prune` when needed.
Transient OpenAI transport failures, HTTP 429 responses and server errors are
retried twice with a short backoff. Final network errors include a Qt error code
and an `X-Client-Request-Id` that can be used to trace requests which never
returned an OpenAI `x-request-id`.

For a global generation request, the assistant inspects the open map and shows
a structured plan. Review it, then click **Approuver et exécuter**. The current global
executor can generate continuous deterministic terrain, choose and apply a base
texture, or procedurally blend three or four textures across every existing tile.
For structured maps, `apply_terrain_layout_on_map` builds continuous corridors,
circular platforms and polygonal areas from normalized map coordinates, then
applies their semantic textures in the same pass. Areas can preserve the existing
micro-relief while being raised or lowered; deterministic edge variation,
automatic slope widening and light smoothing keep global shapes natural and
seamless. This avoids approximating large shapes with dozens of local brush strokes.
The layout may reference up to 16 textures across the map and remaps them to the
native limit of four active layers per chunk. A strict preflight rejects any chunk
that would exceed that limit. Each feature can also add deterministic internal
roughness, so forest and jungle areas keep rolling ground instead of becoming flat
raised plates. `search_textures` is paginated with `offset`/`next_offset` so the
assistant can explore beyond the same first filenames.
`preview_textures` reuses Noggit's BLP renderer to send the model a temporary
visual comparison sheet before it chooses a palette. Preview images are attached
only to the next Responses API request and are not retained in the multi-turn
history. Layout features can blend their semantic texture with the base and vary
corridor width continuously to avoid solid color patches and uniform ribbons.
After validation, `inspect_map_view` can send one scaled viewport capture for an
honest visual report; it is explicitly read-only and never triggers an automatic
second global edit.
`apply_liquid_layout_on_map` can then reuse those corridors or areas to create
real WoW `MH2O` water or ocean surfaces. Liquid levels are sampled
in world space so they remain continuous across chunk and tile boundaries; cells
fully hidden below terrain are cropped automatically. `inspect_map` exposes the
terrain liquid IDs available in the loaded `LiquidType.dbc`, and `validate_map`
reports visible liquid cells, types, surface heights and normalized depths. The
legacy **MCLQ liquids export** setting must be disabled for this tool.
Noggit also rejects a merge that would exceed the renderer limit of 14 active
liquid IDs on one tile; a total replacement can use `replace_existing=true`.
`scatter_assets_on_map` adds the vegetation and mineral decoration after terrain
and water generation. Assets are assigned a canopy, understory, rock or detail
role. Low-frequency deterministic noise creates massifs and clearings, spatial
patches keep species from being mixed uniformly, and scale-aware Poisson-style
spacing simulates competition around large specimens. Height, slope, liquid and
explicit gameplay exclusions form the habitat mask. A batch is capped at 16384
candidates and reports every rejection reason plus counts per region and asset.
For complete three-lane arenas, `create_moba_arena_blueprint` keeps gameplay
topology out of the language model. The model chooses four reviewed textures,
the liquid type, decorative assets and bounded style parameters; the blueprint
returns exact arguments for the existing terrain, liquid and scatter tools. Its
square-map template places fortified bases in opposite corners, side lanes along
the perimeter, mid and river on opposing diagonals, four raised jungle sectors,
twelve protected camp clearings and two epic objective pits. Irregular nested
terrain masses create rocky jungle boundaries; higher-priority paths and camps
cut readable passages through them, while constructed walls remain at the two
bases. Scatter preflight counts only regions that actually intersect a
tile, so a 2x2 arena does not multiply candidates across unrelated tiles.
Generic tools remain available unchanged for non-MOBA maps.
The main viewport also approximates native texture GroundEffects with transient
M2 instances (at most 16 per chunk) so ground cover can be reviewed without
launching WoW. These preview instances are never saved into the ADT and follow
the normal M2 visibility toggle; set `ground_effect_preview=false` in Noggit's
Qt settings to disable them.
The procedural blend follows the terrain height and slope, with continuous noise
to avoid chunk-shaped borders, and reports how many pixels were actually mixed.
The assistant validates every chunk afterward. It processes and saves one tile at
a time; **Annuler** stops between tiles. Global operations are not available through
`Ctrl+Z`, so the approval step explicitly warns before the first save. Local brush
operations remain undoable.

Example:

```text
Transforme cette carte en île naturelle avec des collines et une belle texture
d'herbe. Choisis toi-même les paramètres et propose-moi le plan avant d'agir.
```

```text
Crée une arène verdoyante avec trois voies continues, deux bases, une rivière
centrale avec de l'eau réelle, de la roche sur les pentes et une jungle peuplée
d'arbres, buissons et rochers sans bloquer les voies. Propose le plan avant d'agir,
puis valide la carte.
```

The default model is `gpt-5.6-terra`; set `OPENAI_MODEL` to override it.

## Windows ##
Text in `<brackets>` below are up to your choice but shall be replaced
with the same choice every time the same text is contained.

### MSVC++ ###
Any recent version of Microsoft Visual C++ should work. Be sure to
remember which version you chose as later on you will have to pick
corresponding versions for other dependencies.

### CMake ###
Any recent CMake 3.x version should work. Just take the latest.

### Qt5 ###
Install Qt5 to `<Qt-install>`, downloading a pre-built package from
https://www.qt.io/download-open-source/#section-2.

Note that during installation you only need **one** version of Qt and
also only **one** compiler version. If download size is noticably large
(more than a few hundred MB), you're probably downloading way too much.

### StormLib ###
This step is only required if pulling the dependency from FetchContent is not available.
Download StormLib from https://github.com/ladislav-zezula/StormLib (any
recent version).

* open CMake GUI
* set `CMAKE_INSTALL_PREFIX` (path) to `<Stormlib-install>` (folder should
  not yet exist). No other things should need to be configured.
* open solution with visual studio
* build ALL_BUILD
* build INSTALL
* Repeat for both release and debug.

### MySQL (Optional) ###
Optional, required for MySQL GUID Storage builds.
download MySQL server https://dev.mysql.com/downloads/installer/
and MySQL C++ Connector https://dev.mysql.com/downloads/connector/cpp/
* open CMake GUI
* enable `USE_SQL`
* set `MYSQL_LIBRARY` (path) to `libmysql.lib` from your MYSQL server install.
e.g `"C:/Program Files/MySQL/MySQL Server 8.0/lib/libmysql.lib"`
* set `MYSQLCPPCONN_INCLUDE` (path) to the folder containing `cppconn/driver.h` from your MYSQL Connector C++ install.
e.g `"C:/Program Files/MySQL/Connector C++ 8.0/include/jdbc"`
* set `MYSQLCPPCONN_LIBRARY` (path) to `mysqlcppconn.lib` from your MYSQL Connector C++ install.
e.g `"C:/Program Files/MySQL/Connector C++ 8.0/lib64/vs14/mysqlcppconn.lib"`
* Don't forget to set your SQL settings and enable the feature in the noggit settings menu to use it.

### Noggit ###
* open CMake GUI
* set `CMAKE_PREFIX_PATH` (path) to `"<Qt-install>;<Stormlib-install>"`,
  e.g. `"C:/Qt/5.6/msvc2015;D:/StormLib/install"`
* set `BOOST_ROOT` (path) to `<boost-install>`, e.g. `"C:/local/boost_1_60_0"`
* (**unlikely to be required:**) move the libraries of Boost from where
  they are into `BOOST_ROOT/lib` so that CMake finds them automatically or
  set `BOOST_LIBRARYDIR` to where your lib are (.dll and .lib). Again, this
  is **highly** unlikely to be required.
* set `CMAKE_INSTALL_PREFIX` (path) to an empty destination, e.g. 
  `"C:/Users/blurb/Documents/noggitinstall`
* configure, generate
* open solution with visual studio
* build ALL_BUILD
* build INSTALL
 
To launch noggit you will need the following DLLs from Qt loadable. Install
them in the system, or copy them from `C:/Qt/X.X/msvcXXXX/bin` into the
directory containing noggit.exe, i.e. `CMAKE_INSTALL_PREFIX` configured.

* release: Qt5Core, Qt5OpenGL, Qt5Widgets, Qt5Gui
* debug: Qt5Cored, Qt5OpenGLd, Qt5Widgetsd, Qt5Guid 

## Linux ##
On **Ubuntu** you can install the building requirements using:

```bash
sudo apt install freeglut3-dev libboost-all-dev qt5-default libstorm-dev
```

Compile and build using:

```bash
mkdir build
cd build
cmake ..
make -j $(nproc)
```

Instead of `make -j $(nproc)` you may want to pick a bigger number than
`$(nproc)`, e.g. the number of `CPU cores * 1.5`.

If the build pass correctly without errors, you can go into build/bin/
and run noggit. Note that `make install` will probably work but is not
tested, and nobody has built distributable packages in years.

# SUBMODULES #

To pull the latest version of submodules use the following command at the root directory.

```bash
git submodule update --recursive --remote
```

# CODING GUIDELINES #
File naming rules:

```.hpp``` - is used for header files (C++ language).

```.h``` - is used **only** for header files or modules written in C language.

```.c``` - is used **only** for implementation files or modules written in C language.

```.cpp``` - is used for project implementation files. 

```.inl``` - is used for include files providing template instantiations.

```.ui``` - is used for QT UI definitions (output of QtDesigner/QtCreator).

### Project structure: ###

```/src/Noggit``` - is the main directory hosting .cpp, .hpp, .inl, .ui files of the project.

Within this directory the subdirs should correspond to namespace names (case sensitive). 

File names should use PascalCase (e.g. ```FooBan.hpp```) and either correspond to the type defined in the file,
or represent sematics of the module.

```/src/External``` - is the directory of hosting included libraries and subprojects. This is external or modified
external code, so no rules from Noggit project apply to its content.

```/src/Glsl``` - is the directory to store .glsl shaders for the OpenGL renderer. It is not recommended, 
but not strictly prohibited to inline shader code as strings to ```.cpp``` implementation files.


### Code style ###

Following is an example for file `src/Noggit/Ui/FooBan.hpp`. 

```cpp
#ifndef INCLUDE_GUARD_BASED_ON_FILENAME
#define INCLUDE_GUARD_BASED_ON_FILENAME
// We do not use #pragma once in headers as it is technically not cross-platform.
// Use include guards instead. For example, CLion IDE creates them automatically on .hpp file creation.

// <> are prefered for includes.
// Local imports go here
#include <SomeLocalFile.hpp>

// Lib imports go here
#include <external/SomeLibCode.hpp

// STL imports go here
#include <string>
#include <mutex>
#include <vector> // etc

// Forward declarations in headers are encouraged. That prevents type leaking into bigger scopes
// Also reduces compile time
namespace Parent::SomeOtherChild
{
  class ForwardDeclaredClass;
}

// Namespaces are defined as PascalCase names. Namespace concatenation for nested namespaces
// is adviced, but not strictly enforced.
namespace Parent::Child
{
  // types are name in PascalCase,
  class Test : public TestBase
  {
    public:
      Test();
     
      int x; // public fields like that are discourged, but occur here and there through the project. 
      // Subject to refactoring.
      
      // methods are named in camelCase.
      // trivial getter methods are declared in the header file.
      int somePrivateMember() { return _some_private_member; } const;

      // trivial setters are declared in the header file. Preceded by "set" prefix.
      void setSomePrivateMember(int a) { _some_private_member = a; };
    
    // private members are snake lower case, separated by underscore, preceded by underscore to indicate they're private.
    private:
      int _some_private_member;
      ForwardDeclaredClass* _some_other_private_member_using_forward_decl;
      std::mutex _mutex;

    // static methods

    private:
      static void someStaticMethod();
    
  };
}

#endif
```

Following is an example for file `src/Noggit/Ui/FooBan.cpp`.

```cpp
// the header of this .cpp comes first
// <> are prefered for includes.
#include <Noggit/Ui/FooBan.hpp>

// same order of includes as in header.

using namespace Parent::Child;

Test::Test()
: TestBase("some_arg")
, _some_private_member(0)
, _some_other_private_member_using_forward_decl(new ForwardDeclaredClass()) // do not forget to import ForwardDeclaredClass in .cpp
{
// body of ctor
}

void Test::someStaticMethod()
{
// local variables are named in snake_case, no preceding underscore.
int local_var = 0;

// preceding underscore is used on variables that are used for RAII patterns, such as scoped stuff (e.g. a scoped mutex)
std::lock_guard<std::mutex> _lock (_mutex); // _lock is never accessed later, it just needs to live as long as the scope lives.
// So, it has an underscore prefix.

someFunc(local_var); // free floating functions use the same naming rules as methods
}
```

Additional examples:

```cpp

constexpr unsigned SOME_CONSTANT = 10; // constants are named in SCREAMING_CASE
#define SOME_MACRO // macro definitions are named in SCREAMING_CASE

```
