-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. A rack of three knobs and a mix slider: each knob is a row with an
-- icon, a monospaced name and a slider whose value prints with a unit, plus a
-- flat switch and an output meter. Values live in self, so the console can
-- read them (print(self_probe) style) while you drag. Joins the app's card
-- stack through _G[__cui_slot]; widget(nil) restores the plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.cutoff = self.cutoff or 60                  -- knob values survive frames in self
  self.reso   = self.reso or 25
  self.drive  = self.drive or 4
  self.mix    = self.mix or 0.8
  self.flat   = self.flat or false
  local knobs = {                                  -- name, value, lo, hi, printf format
    { 'cutoff', self.cutoff, 0, 100, '%0.0f Hz' },
    { 'reso',   self.reso,   0, 100, '%0.0f %%' },
    { 'drive',  self.drive,  0, 12,  '%+0.1f dB' },
  }
  ui.card({ gap = 10 }, function()
    ui.row({ gap = 8, align = 'center' }, function()
      ui.icon('sliders-horizontal', 16)
      ui.heading('knob rack', 3)
      ui.spacer()
      ui.badge(self.flat and 'flat' or 'warm', { tone = self.flat and 'info' or 'warning', dot = true })
    end)
    for _, k in ipairs(knobs) do
      ui.row({ gap = 8, align = 'center' }, function()
        ui.icon('activity', 14)
        ui.text(k[1], { font = 'mono', width = 64 })
        k[2] = ui.slider(k[1], k[2], { min = k[3], max = k[4], format = k[5], show_value = true, width = 'fill' })
      end)
    end
    self.cutoff, self.reso, self.drive = knobs[1][2], knobs[2][2], knobs[3][2]
    self.mix = ui.slider('mix', self.mix, { min = 0, max = 1, show_value = true })
    self.flat = ui.switch('Flat', self.flat)
    ui.progress(self.flat and 0.5 or self.mix, { tone = 'accent', show_value = true, label = 'output' })
  end)
end)
