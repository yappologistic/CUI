-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. A 3-lane, 4-step sequencer grid: checkboxes pattern the steps, prev
-- and next move a step marker, and Random and Clear re-pattern everything at
-- once. The pattern is a table of tables in self, so the console can print it.
-- Joins the app's card stack through _G[__cui_slot]; widget(nil) restores the
-- plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.grid = self.grid or {}                      -- grid[lane][step], survives frames in self
  self.step = self.step or 1
  local lanes = { 'kick', 'snare', 'hat' }
  ui.card({ gap = 8 }, function()
    ui.row({ gap = 8, align = 'center' }, function()
      ui.icon('radio', 16)
      ui.heading('step sequencer', 3)
      ui.spacer()
      ui.badge('step ' .. self.step .. '/4', { tone = 'violet', solid = true })
    end)
    for _, lane in ipairs(lanes) do
      self.grid[lane] = self.grid[lane] or {}
      ui.row({ gap = 8, align = 'center' }, function()
        ui.text(lane, { font = 'mono', width = 56 })
        for s = 1, 4 do
          self.grid[lane][s] = ui.checkbox(tostring(s), self.grid[lane][s])
        end
      end)
    end
    ui.row({ gap = 8 }, function()
      if ui.button('prev', { variant = 'outline', size = 'sm' }) then self.step = self.step > 1 and self.step - 1 or 4 end
      if ui.button('next', { variant = 'outline', size = 'sm' }) then self.step = self.step < 4 and self.step + 1 or 1 end
      if ui.button('Random', { variant = 'ghost', size = 'sm' }) then
        for _, lane in ipairs(lanes) do for s = 1, 4 do self.grid[lane][s] = math.random() < 0.4 end end
      end
      if ui.button('Clear', { variant = 'ghost', size = 'sm' }) then
        for _, lane in ipairs(lanes) do for s = 1, 4 do self.grid[lane][s] = false end end
      end
    end)
  end)
end)
