# Launch checklist

What has to be true before a module goes from hidden to shipping, and before a
release is tagged. Written down because most of these were learned by getting
them wrong: every item below has a module's name attached to it somewhere.

A module can sit hidden in `plugin.json` for as long as it takes. Nothing here
applies until it comes out.

## The module

- [ ] **Panel published, not generated.** If the artwork came from a design
      file, `python3 tools/figma_panel_template.py --publish <key>` ships it and
      hides the placement guides. `panel_reticules.py` *generates* a panel from
      the widget source and emits plates and screen only — run it on a finished
      panel and it draws over the artwork. Add the key to `FINISHED` in
      `panel_reticules.py` so the bare sweep skips it.
      See [panel-design.md](panel-design.md), [panel-reticules.md](panel-reticules.md).

- [ ] **Every control sits on a guide.** `--publish` reports `N/M guides hidden`
      and names any control with no guide in the art. One that is genuinely
      placed by hand is fine; anything else means the code and the design have
      drifted.

- [ ] **No screws.** No `createWidget<ScrewSilver>` or equivalent, ever.

- [ ] **`drawPreview()` on every custom display.** The VCV Library builds its
      thumbnail with `module == NULL`. Without one the browser shows a dark
      slab. `./tools/screenshots.sh` renders through the same path, so a missing
      preview is visible there.
      See [browser-preview-pattern.md](browser-preview-pattern.md).

- [ ] **Screens in Share Tech Mono, panels in Figtree.** `sfs::screenFontFace()`
      for the former, and one size (`sfs::TYPE_SCREEN`). Reaching for
      `sfs::panelFont()` inside a display is the mistake four modules made.

- [ ] **Scales come from `src/scales.hpp`.** Any SCALE control uses the shared
      canonical list, so a SCALE CV is interchangeable across the plugin. Never
      a local table. See [scales.md](scales.md).

- [ ] **Enums are append-only.** Params, inputs, outputs and lights serialise by
      index. Retire in place with a `"(retired)"` label; never delete or insert.

- [ ] **Static analysis is clean.** `./tools/cppcheck.sh` — see below. This is a
      release-prep check, not something to run on every build.

## The metadata

- [ ] **`hidden` removed** from the module's `plugin.json` entry.

- [ ] **Tags are real Rack tags.** The Library rejects anything not on its list,
      and it rejects it at submission, after the release is cut. "Drone" is not
      a tag; that one nearly shipped.

- [ ] **Description is one short line**, about 85 characters at most. It is a
      browser subtitle, not a summary.

- [ ] **An expander is shipped with its host.** A module whose tooltip points at
      an expander, or whose function needs one, ships in the same release.
      Slide went out with its per-string outputs unreachable.

## The documentation

- [ ] **README section and Contents entry**, in the format the others use.
- [ ] **Screenshot**: `./build.sh prod && ./tools/screenshots.sh`.
- [ ] **CLAUDE.md module list** gains an entry.
- [ ] **No em dashes**, and plain direct language.

## The release

- [ ] `./tools/cppcheck.sh` clean.
- [ ] Version bumped in `plugin.json`.
- [ ] `./build.sh prod` compiles.
- [ ] `./tools/screenshots.sh` if any panel art changed.
- [ ] Commit `Release X.Y.Z: …`, annotated tag `vX.Y.Z`, `git push origin master
      --follow-tags`.
- [ ] `gh release create` with title and notes **only**. No `.vcvplugin`
      binaries attached — the Library builds from source.

## Static analysis

The Library runs cppcheck on every submission and opens an issue with whatever
it finds. Run it before they do — but **only when preparing a release**. It
takes a couple of minutes over the whole tree, it has nothing useful to say
about a change that has not been finished yet, and running it on every dev build
just makes the build slow enough to stop wanting to run.

```bash
./tools/cppcheck.sh          # exit status is the number of findings in our code
```

It is not a formality. The first report ([#11](https://github.com/stuart78/SignalFunctionSet-VCV-Rack/issues/11))
contained two real bugs that testing had not surfaced:

* **Fill's `.txt` importer never assigned `set.taste`.** `sfs::Taste` is a plain
  struct, so an imported bank carried whatever was on the stack — including the
  mask of drums the engine must not touch. The pattern played; it just played
  wrong, occasionally, in a way nobody would think to test for.
* **`sfs_lut::hann()` read one element past its table** at a phase of exactly
  1.0. The weight there is zero, so it never produced a wrong number, and no
  test could have caught it.

Both are the shape of thing static analysis is *for*: correct on every path
anyone would exercise by hand.

Two habits that keep the report readable:

* **Fix the declaration, not just the call site.** `LibSet::taste` got a
  `= {}` as well as an assignment in the importer, so the next code path cannot
  repeat it.
* **Silence false positives rather than living with them.** cppcheck reports
  `x >> 31` on an `int32_t` as undefined behaviour; it is not — the shift count
  is in range and right-shifting a negative value is implementation-defined. It
  was rewritten anyway, to a form that compiles to the identical instruction,
  because a checker that cries wolf on every submission is how a real finding
  gets missed. Where a rewrite is not free, use an inline
  `// cppcheck-suppress <id>` with a comment saying why.

`src/msfa`, `dr_wav.h` and `dr_flac.h` are vendored. The script reports msfa
separately (we do patch it) and filters the two headers entirely, since they
alone produce over a hundred notes that are not ours to answer for.
