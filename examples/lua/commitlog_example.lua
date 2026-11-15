-- Paste into the Lua console of examples/repl or the gallery's Developer tools
-- page. A commit-log window: filter buttons, a scrolling list of rows with a
-- hash in code, a link-styled message, +/− badges and merge markers, plus a
-- button that appends a synthetic commit. The console sandbox has no io,
-- require or os.execute, so it cannot read the real git history — this log
-- lives in the script's own self.commits. Joins the app's card stack through
-- _G[__cui_slot]; widget(nil) restores the plain app card.

widget(function(ui, self)
  local app_card = _G[__cui_slot]                  -- the app's original function for this slot
  if app_card then app_card(ui, self) end          -- the card stack the app already runs
  self.commits = self.commits or {                 -- the log lives in the script's own state
    { h = 'a91f3c2e', msg = 'docs: the envelope editor',  who = 'maya',  add = 12, del = 3,  merge = false },
    { h = '7be04d19', msg = 'fix the grid pad hit test',  who = 'jonas', add = 4,  del = 1,  merge = false },
    { h = 'c3d8a7f0', msg = 'Merge branch "knobs"',       who = 'aiko',  add = 0,  del = 0,  merge = true },
    { h = '1f22b9e4', msg = 'feat: resizable cards',      who = 'priya', add = 48, del = 12, merge = false },
  }
  self.filter = self.filter or 'all'
  self.sel    = self.sel or 1
  self.next   = self.next or 1
  ui.card({ gap = 8 }, function()
    ui.row({ gap = 8, align = 'center' }, function()
      ui.icon('git-branch', 16)
      ui.heading('commit log', 3)
      ui.spacer()
      for _, f in ipairs{ 'all', 'commits', 'merges' } do
        if ui.button(f, { variant = 'ghost', size = 'sm', selected = self.filter == f }) then self.filter = f end
      end
    end)
    ui.scroll({ id = 'log', height = 200, fade_edges = true, gap = 4 }, function()
      for i, c in ipairs(self.commits) do
        if self.filter == 'all' or (self.filter == 'merges') == c.merge then
          ui.row({ gap = 8, align = 'center' }, function()
            if c.merge then ui.icon('git-branch', 14) end
            ui.code(c.h)
            if ui.button(c.msg, { variant = 'link' }) then self.sel = i end
            if c.merge then ui.badge('merge', { tone = 'info', size = 'sm' })
            else ui.badge('+' .. c.add, { tone = 'success', size = 'sm' }); ui.badge('-' .. c.del, { tone = 'danger', size = 'sm' }) end
            ui.spacer()
            ui.muted(c.who)
          end)
        end
      end
    end)
    ui.row({ gap = 8, align = 'center' }, function()
      if ui.button('+1 commit', { variant = 'outline', size = 'sm' }) then
        self.next = self.next + 1
        table.insert(self.commits, 1, { h = string.format('%08x', self.next * 2654435761),
                                        msg = 'chore: tweak ' .. self.next, who = 'script',
                                        add = self.next, del = 0, merge = false })
      end
      ui.secondary('row ' .. self.sel .. ' selected')
    end)
  end)
end)
