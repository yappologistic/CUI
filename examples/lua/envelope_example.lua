-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. An ADSR envelope editor: four sliders, and the stage bars below them
-- grow with their values, so the shape of the envelope reads at a glance. The
-- badge names the shape a preset would give. Joins the app's card stack
-- through _G[__cui_slot]; widget(nil) restores the plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.a = self.a or 0.2                           -- attack, decay, sustain, release in self
  self.d = self.d or 0.3
  self.s = self.s or 0.7
  self.r = self.r or 0.4
  local shape = (self.a > 0.5 and self.r > 0.5) and 'swell'
                or (self.a < 0.15 and self.r < 0.15) and 'pluck' or 'pad'
  ui.card({ gap = 8 }, function()
    ui.row({ gap = 8, align = 'center' }, function()
      ui.icon('activity', 16)
      ui.heading('envelope', 3)
      ui.spacer()
      ui.badge(shape, { tone = 'teal', solid = true })
    end)
    self.a = ui.slider('A', self.a, { min = 0, max = 1, show_value = true })
    self.d = ui.slider('D', self.d, { min = 0, max = 1, show_value = true })
    self.s = ui.slider('S', self.s, { min = 0, max = 1, show_value = true })
    self.r = ui.slider('R', self.r, { min = 0, max = 1, show_value = true })
    ui.row({ gap = 4, align = 'end' }, function()   -- the stage bars rise with their values
      ui.progress(self.a, { height = 8 + 44 * self.a, tone = 'accent', width = 'fill' })
      ui.progress(self.d, { height = 8 + 44 * self.d, tone = 'info', width = 'fill' })
      ui.progress(self.s, { height = 8 + 44 * self.s, tone = 'success', width = 'fill' })
      ui.progress(self.r, { height = 8 + 44 * self.r, tone = 'violet', width = 'fill' })
    end)
    ui.code(string.format('A %.2f  D %.2f  S %.2f  R %.2f', self.a, self.d, self.s, self.r))
    if ui.button('Pluck preset', { variant = 'outline', size = 'sm' }) then
      self.a = 0.05 self.d = 0.18 self.s = 0.15 self.r = 0.08
    end
  end)
end)
