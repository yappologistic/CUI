-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. The EXPAND card joins the app's own card: the wrapper first calls the
-- app's function for this slot (_G[__cui_slot]), then draws its card after it.
-- The text field starts as "e"; pressing EXPAND turns it into "E X P A N D".
-- widget(nil) restores the plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.expand = self.expand or 'e'                 -- the text starts as "e"
  ui.card({ gap = 8 }, function()
    ui.label('press EXPAND to stretch the text')
    self.expand = ui.textfield('expand', self.expand)
    if ui.button('EXPAND', { variant = 'primary' }) then self.expand = 'E X P A N S I O N From The R E P L' end
  end)
end)
