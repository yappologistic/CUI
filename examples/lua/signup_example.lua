-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. The sign-up card joins the app's own card through _G[__cui_slot]: the
-- wrapper first calls the app's function for this slot, then draws its card
-- after it. Text fields, a password, a strength bar and a checkbox gate the
-- button; submitting raises a toast. widget(nil) restores the plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.name  = self.name  or ''
  self.pass  = self.pass  or ''
  self.agree = self.agree or false
  ui.card({ gap = 8 }, function()
    ui.heading('Sign up', 3)
    self.name = ui.textfield('name', self.name, { placeholder = 'your name', prefix = '@', clearable = true })
    self.pass = ui.textfield('pass', self.pass, { password = true, hint = 'at least 6 characters' })
    local ok = 0
    for _ = 1, math.min(#self.pass, 6) do ok = ok + 1 end
    ui.progress(ok / 6, { tone = ok >= 6 and 'success' or 'warning', label = 'strength' })
    self.agree = ui.checkbox('I agree', self.agree, { description = 'to the made-up terms' })
    local ready = self.agree and #self.name > 0 and #self.pass >= 6
    if ui.button('Create account', { variant = 'primary', full_width = true, disabled = not ready }) then
      ui.toast('welcome, ' .. self.name, { tone = 'success', description = 'account created' })
    end
  end)
end)
