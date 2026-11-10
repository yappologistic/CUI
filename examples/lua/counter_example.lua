-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. The counter card joins the app's own card through _G[__cui_slot]: the
-- wrapper first calls the app's function for this slot, then draws its card
-- after it. Buttons move the number; the badge tone follows its sign.
-- widget(nil) restores the plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.n = self.n or 0                             -- the number survives frames in self
  local tone = self.n > 0 and 'success' or self.n < 0 and 'danger' or 'neutral'
  ui.card({ gap = 8 }, function()
    ui.row({ gap = 8, align = 'center' }, function()
      ui.icon('activity', 16)
      ui.heading('Counter', 3)
      ui.spacer()
      ui.badge(tostring(self.n), { tone = tone, solid = true })
    end)
    ui.text(tostring(self.n), { font = 'mono', size = 28, align = 'center', width = 'fill' })
    ui.row({ gap = 8 }, function()
      if ui.button('Minus', { variant = 'outline', icon = 'minus' }) then self.n = self.n - 1 end
      if ui.button('Plus', { variant = 'primary', icon = 'plus' }) then self.n = self.n + 1 end
      if ui.button('Reset', { variant = 'ghost' }) then self.n = 0 end
    end)
  end)
end)
