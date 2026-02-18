# Level Editor Shortcuts

Unreal Engine 5 editor plugin that adds keyboard shortcuts for faster level editing: drag-to-move, scroll-to-rotate, snap-to-ground, transform copy/paste, and more.

## Installation

1. Download or clone this repo into your project's `Plugins/` folder
2. Rebuild your project
3. Enable the plugin in Edit > Plugins if not auto-enabled

## Shortcuts

### Movement & Transform

| Shortcut | Action |
|----------|--------|
| 1 / 2 / 3 | Switch to Move / Rotate / Scale gizmo |
| Q + Drag | Move selected actor(s) horizontally (respects local/world space) |
| E + Drag | Move selected actor(s) vertically (respects local/world space) |
| R + Drag | Scale selected actor(s) uniformly |
| Q + Scroll | Rotate selected actor(s) around Z axis (respects rotation snap) |
| Shift + Q + Scroll | Rotate ignoring snap |
| Shift + Rotate drag | Temporarily bypass rotation snap while dragging the rotation gizmo |

### Grid Snap

| Shortcut | Action |
|----------|--------|
| G (tap) | Toggle grid snap on/off |
| G + Scroll | Change grid snap size |

### Transform Copy/Paste

| Shortcut | Action |
|----------|--------|
| Ctrl + C | Copy transform of selected actor (normal copy still works) |
| Ctrl + T | Paste location + rotation to selected actor(s), preserving scale |
| Ctrl + D | Duplicate in place (no offset) |

### Snap to Ground

| Shortcut | Action |
|----------|--------|
| Ctrl + B | Snap to ground, inheriting surface slope rotation |
| Shift + B | Snap to ground, keeping world-up orientation |

Both snap modes use the mesh/collision bounds to place the bottom of the object on the surface. Traces use `ECC_Visibility` and skip query-only colliders.

### Paste to Folder

| Shortcut | Action |
|----------|--------|
| Ctrl + Shift + V | Paste clipboard actors into the same World Outliner folder as the selected actor |

## Notes

- Q/E/R drag hides the cursor and provides infinite movement range (cursor warps back to start)
- All drag operations create a single undo transaction (one Ctrl+Z undoes the entire drag)
- Movement respects the current grid snap size and local/world coordinate system
- 1-2-3 gizmo switching is disabled in Landscape/Foliage modes (those use number keys for tools)
- Works in the Level Editor viewport only - Blueprint editor and other viewports keep their default bindings

## Compatibility

Developed on UE 5.6. Uses standard editor APIs (`GLevelEditorModeTools`, `ULevelEditorViewportSettings`, etc.) so should work on most UE5 versions. The Paste to Folder feature uses Windows `SendInput` API.

## License

MIT
