-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. The states card joins the app's own card through _G[__cui_slot]: the
-- wrapper first calls the app's function for this slot, then draws its card
-- after it. It exercises the control states: loading and disabled buttons, an
-- indeterminate bar, and an indeterminate checkbox. widget(nil) restores the
-- plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.busy  = self.busy  or false                 -- Run flips it, Stop clears it
  self.ready = self.ready or false
  self.all   = self.all   or false
  ui.card({ gap = 8 }, function()
    ui.heading('States', 3)
    if ui.button('Run task', { variant = 'primary', icon = 'play', loading = self.busy }) then self.busy = true end
    if self.busy then
      ui.progress(0, { indeterminate = true, label = 'working...' })
      if ui.button('Stop', { variant = 'outline' }) then self.busy = false end
    end
    self.ready = ui.switch('Ready', self.ready)
    if ui.button('Publish', { variant = 'primary', disabled = not self.ready }) then
      ui.toast('published', { tone = 'success' })
    end
    self.all = ui.checkbox('Select all', self.all, { indeterminate = self.busy, disabled = self.busy })
  end)
end)
