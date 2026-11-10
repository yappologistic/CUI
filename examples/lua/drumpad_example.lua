-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. Eight finger pads in a grid: each hit bumps its counter, lights up as
-- the last hit and raises a toast. Joins the app's card stack through
-- _G[__cui_slot]; widget(nil) restores the plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.hits = self.hits or {}                      -- per-pad hit counts survive frames in self
  self.last = self.last or 0
  local pads = { 'kick', 'snare', 'clap', 'hat', 'tom', 'rim', 'cow', 'shaker' }
  local total = 0
  for _, h in pairs(self.hits) do total = total + h end
  ui.card({ gap = 10 }, function()
    ui.row({ gap = 8, align = 'center' }, function()
      ui.icon('audio-lines', 18)
      ui.heading('drumpad', 3)
      ui.spacer()
      ui.badge(self.last > 0 and pads[self.last] or 'silent', { tone = self.last > 0 and 'success' or 'neutral', dot = true })
      ui.badge(total .. ' hits', { tone = 'violet', solid = true })
    end)
    ui.box({ dir = 'grid', columns = 4, gap = 8 }, function()
      for i, name in ipairs(pads) do
        if ui.button(name, { variant = self.last == i and 'primary' or 'secondary',
                             icon = 'dot', full_width = true,
                             tooltip = self.hits[i] and (self.hits[i] .. ' hits') or 'hit me' }) then
          self.last = i
          self.hits[i] = (self.hits[i] or 0) + 1
          ui.toast(name, { tone = 'success', description = self.hits[i] .. ' hits', duration = 1.5 })
        end
      end
    end)
    if ui.button('Clear', { variant = 'ghost', size = 'sm' }) then self.hits = {}; self.last = 0 end
  end)
end)
