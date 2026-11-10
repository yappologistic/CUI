-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. The scratchpad card joins the app's own card through _G[__cui_slot]:
-- the wrapper first calls the app's function for this slot, then draws its
-- card after it. A text area, a live word count in a badge, and buttons that
-- clear the pad or toast a save. widget(nil) restores the plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.notes = self.notes or 'notes from the console'
  local words = 0
  for _ in self.notes:gmatch('%S+') do words = words + 1 end
  ui.card({ gap = 8 }, function()
    ui.row({ gap = 8, align = 'center' }, function()
      ui.icon('square-pen', 16)
      ui.heading('Scratchpad', 3)
      ui.spacer()
      ui.badge(words .. ' words', { tone = 'info', dot = true })
    end)
    self.notes = ui.textarea('notes', self.notes, { placeholder = 'type here' })
    ui.row({ gap = 8 }, function()
      if ui.button('Clear', { variant = 'outline' }) then self.notes = '' end
      if ui.button('Save', { variant = 'primary' }) then
        ui.toast('saved', { tone = 'success', description = words .. ' words' })
      end
    end)
  end)
end)
