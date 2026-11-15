-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. Two cards side by side that you resize from inside: + and - widen and
-- narrow each card (its width lives in self.wa / self.wb, so the console can
-- read and set them too), and hide fades a card out through the muted option.
-- Joins the app's card stack through _G[__cui_slot]; widget(nil) restores the
-- plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.wa = self.wa or 240                         -- card widths live in self, try print(self_probe)
  self.wb = self.wb or 320
  local function panel(name, wkey, tone)
    ui.card({ gap = 8, width = self[wkey], muted = self['hide' .. name] or false }, function()
      ui.row({ gap = 6, align = 'center' }, function()
        ui.text('card ' .. name, { font = 'medium' })
        ui.spacer()
        ui.badge(self[wkey] .. 'pt', { tone = tone, solid = true })
      end)
      ui.secondary('resize me from inside')
      ui.row({ gap = 6 }, function()
        if ui.button('- ' .. name, { size = 'sm', variant = 'outline' }) then
          self[wkey] = math.max(160, self[wkey] - 40)
        end
        if ui.button('+ ' .. name, { size = 'sm', variant = 'outline' }) then
          self[wkey] = math.min(420, self[wkey] + 40)
        end
        if ui.button('hide ' .. name, { size = 'sm', variant = 'ghost' }) then
          self['hide' .. name] = not (self['hide' .. name] or false)
        end
      end)
    end)
  end
  ui.row({ gap = 12, align = 'start', wrap = true }, function()
    panel('A', 'wa', 'info')
    panel('B', 'wb', 'violet')
  end)
end)
