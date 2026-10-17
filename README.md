# Colosseum Native

Pokémon Colosseum (GameCube, USA) as a native Windows and Android app, built by
**static recompilation**: the game's PowerPC code is translated ahead of time to C
with [DolRecomp](https://github.com/ExpansionPak/DolRecomp) and runs on the
[ModernGekko](https://github.com/ExpansionPak/ModernGekko) runtime. The goal is the
highest-fidelity way to play it: high internal resolution, MSAA/SSAA, 16× anisotropic
filtering, widescreen and ultrawide, texture packs, and up to 144 fps.

**Status: boots to the title screen.** The full main.dol recompiles (623,456
instructions, 0 unknown) and runs entirely as native code (0 interpreter fallbacks,
0 failed self-modifying-code checks) through the Genius Sonority logo and intro to
PRESS START. See [docs/PLAN.md](docs/PLAN.md).

You need your own copy: **Pokémon Colosseum (USA), `GC6E01`**, as ISO or CISO.
No game data is included in or downloaded by this repository.

## Working with the disc

```sh
python3 tools/disc.py ciso2iso "Pokemon Colosseum (USA).ciso" iso/colosseum.iso
python3 tools/disc.py verify   iso/colosseum.iso
DOLRECOMP=path/to/dolrecomp scripts/recompile.sh iso/colosseum.iso
```

Settings live in `ColosseumNative.ini` next to the app; the default with every
option explained is [config/ColosseumNative.ini](config/ColosseumNative.ini).

## License

GPL-3.0 (DolRecomp and ModernGekko are GPL-3.0). Pokémon Colosseum © Nintendo,
Creatures, GAME FREAK, Genius Sonority, The Pokémon Company. This project is not
affiliated with or endorsed by them.
