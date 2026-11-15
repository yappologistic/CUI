-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. The mixer card joins the app's own card through _G[__cui_slot]: the
-- wrapper first calls the app's function for this slot, then draws its card
-- after it. Sliders drive a master bar; the mute switch zeroes it.
-- widget(nil) restores the plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.vol  = self.vol  or 0.7                     -- slider values survive frames in self
  self.bass = self.bass or 0.5
  self.treb = self.treb or 0.4
  self.mute = self.mute or false
  ui.card({ gap = 8 }, function()
    ui.row({ gap = 8, align = 'center' }, function()
      ui.icon('audio-lines', 18)
      ui.heading('mixer', 3)
      ui.spacer()
      ui.badge(self.mute and 'muted' or 'live', { tone = self.mute and 'danger' or 'success', dot = true })
    end)
    self.vol  = ui.slider('vol',  self.vol,  { min = 0, max = 1, show_value = true })
    self.bass = ui.slider('bass', self.bass, { min = 0, max = 1, step = 0.05 })
    self.treb = ui.slider('treb', self.treb, { min = 0, max = 1, step = 0.05 })
    self.mute = ui.switch('Mute', self.mute)
    ui.progress(self.mute and 0 or self.vol, { tone = 'accent', show_value = true, label = 'master' })
  end)
end)
