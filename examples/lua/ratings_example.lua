-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. The ratings card joins the app's own card through _G[__cui_slot]: the
-- wrapper first calls the app's function for this slot, then draws its card
-- after it. Three star ratings feed an average bar. widget(nil) restores the
-- plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.r1 = self.r1 or 3                           -- stars survive frames in self
  self.r2 = self.r2 or 4
  self.r3 = self.r3 or 2
  ui.card({ gap = 8 }, function()
    ui.heading('Rate the parts', 3)
    ui.row({ gap = 8, align = 'center' }, function() ui.text('layout'); ui.spacer(); self.r1 = ui.rating('r1', self.r1, 5) end)
    ui.row({ gap = 8, align = 'center' }, function() ui.text('widgets'); ui.spacer(); self.r2 = ui.rating('r2', self.r2, 5) end)
    ui.row({ gap = 8, align = 'center' }, function() ui.text('docs');    ui.spacer(); self.r3 = ui.rating('r3', self.r3, 5) end)
    ui.separator()
    ui.row({ gap = 8, align = 'center' }, function()
      ui.badge(tostring(self.r1 + self.r2 + self.r3) .. ' / 15', { tone = 'teal', dot = true })
      ui.progress((self.r1 + self.r2 + self.r3) / 15, { tone = 'teal', show_value = true, label = 'average' })
    end)
  end)
end)
