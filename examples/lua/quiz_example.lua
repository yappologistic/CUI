-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. The quiz card joins the app's own card through _G[__cui_slot]: the
-- wrapper first calls the app's function for this slot, then draws its card
-- after it. Buttons answer the question; the badge and the bar follow the score.
-- widget(nil) restores the plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.pick  = self.pick  or 0                     -- 0 = no answer yet, 1..3 = the chosen one
  self.score = self.score or 0
  ui.card({ gap = 8 }, function()
    ui.heading('Which renderer needs no GPU?', 3)
    ui.muted('tap an answer; only Software scores')
    local names = { 'Software', 'OpenGL', 'Vulkan' }
    for i, n in ipairs(names) do
      if ui.button(n, { variant = self.pick == i and 'primary' or 'secondary', full_width = true }) then
        self.pick = i
        if i == 1 then self.score = self.score + 1; ui.toast('correct', { tone = 'success' })
        else ui.toast('not this one', { tone = 'warning' }) end
      end
    end
    ui.separator()
    ui.row({ gap = 8, align = 'center' }, function()
      ui.badge('score ' .. self.score, { tone = 'violet', solid = true })
      ui.progress(math.min(1, self.score / 3), { tone = 'accent', show_value = true })
    end)
  end)
end)
