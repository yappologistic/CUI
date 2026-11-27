# Navigating time charts

The wheel moves the content under the pointer. On the time ruler, a vertical wheel zooms even in a deep capture. Over the plot, it scrolls rows if they overflow and zooms if they all fit. Reaching the last row doesn't turn the next notch into a zoom.

| Widget | Wheel on time ruler | Wheel on left gutter | Wheel on plot | Horizontal wheel or Shift+wheel | Ctrl/Cmd+vertical wheel | Drag |
|---|---|---|---|---|---|---|
| `cui_flame_graph` | Zoom | No gutter | Scroll overflowing stack rows; otherwise zoom | Pan time | Zoom | Primary drag pans time and rows |
| `cui_trace_view` | Zoom, including the marker strip | Scroll lanes | Scroll overflowing lanes; otherwise zoom | Pan time | Zoom | Primary drag pans time and lanes; Shift+drag measures; pressing a lane name folds it |
| `cui_timeline_editor` | Zoom | Scroll tracks | Scroll overflowing tracks; otherwise zoom | Pan time | Zoom | Middle drag pans time and tracks; primary drag still scrubs, selects and edits |

- **Zoom** preserves the time under the pointer unless constrained by the data bounds or time precision. In the gutter it anchors the left edge of the plot. The gutter takes precedence over the ruler in the upper-left corner; a gutter whose rows fit doesn't scroll or zoom.
- **Pan time** moves the horizontal window without changing its duration. For a gesture with two wheel axes, Ctrl/Cmd and a nonzero vertical delta zoom first. Otherwise Shift pans using whichever delta has the larger magnitude. Without modifiers, a horizontal delta at least as large as the vertical one pans; a predominantly vertical gesture follows the table. Pure horizontal motion pans even with Ctrl held.
- Only the visible chart, including the ruler, takes the wheel. A disabled chart leaves it to its container. These bindings don't change keyboard, selection or edit commands.

**Following a live capture:** The flame graph and trace viewer don't own a live clock. If an application keeps a time window at “now”, it can turn that behavior off when the user actually pans or zooms time. Scrolling rows or measuring an interval need not stop follow. `view_changed` alone isn't enough to distinguish these cases: fitting to new data can also change a view. The timeline editor instead pages a moving playhead into view.
