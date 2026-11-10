-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. This card joins the app's own card through _G[__cui_slot]: the wrapper
-- first calls the app's function for this slot, then draws its card after it.
-- It exercises the card options themselves: hoverable, selected and muted,
-- plus code, kbd and secondary text. widget(nil) restores the plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.sel = self.sel or false
  ui.card({ gap = 8, hoverable = true, selected = self.sel, muted = not self.sel }, function()
    ui.row({ gap = 6, align = 'center' }, function()
      ui.icon('sparkles', 16)
      ui.text('styled card', { font = 'medium' })
      ui.spacer()
      ui.badge(self.sel and 'selected' or 'plain', { tone = 'accent', solid = true })
    end)
    ui.secondary('hovering lights it; the switch mutes it')
    self.sel = ui.switch('Selected', self.sel)
    ui.code("ui.card({ hoverable = true, selected = ..., muted = ... })")
    ui.kbd('Tab')
  end)
end)
