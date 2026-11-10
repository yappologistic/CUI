-- Soil pH and nutrient availability chart for the CUI Lua console.
-- Run:  CUI.seclorum/build/examples/repl cui_phi_nutrients.lua
-- Port of BUI ph_nutrients.lua to CUI's immediate-mode Lua bindings: no
-- canvas/tree/tabs widgets in the repl, so the Truog plot is monospace text
-- (pH bands + Gaussian availability sparklines), tabs are toggle buttons and
-- the Table tab is an expandable button list editing the same model.
-- Band ranges are illustrative. SPDX-License-Identifier: GPL-2.0-or-later

local PH_MIN,PH_MAX,W,COL,NPTS=4.0,10.0,60,110,61
local DEF_N={
 {name="Nitrogen",ph_min=4.0,ph_max=10.0,color={0.30,0.55,0.25}},
 {name="Phosphorus",ph_min=5.5,ph_max=8.0,color={0.25,0.50,0.35}},
 {name="Potassium",ph_min=4.5,ph_max=9.5,color={0.40,0.55,0.20}},
 {name="Sulfur",ph_min=4.0,ph_max=9.5,color={0.45,0.55,0.30}},
 {name="Calcium",ph_min=5.5,ph_max=9.5,color={0.35,0.50,0.45}},
 {name="Magnesium",ph_min=5.5,ph_max=8.5,color={0.30,0.45,0.30}},
 {name="Iron",ph_min=4.0,ph_max=6.5,color={0.60,0.35,0.15}},
 {name="Manganese",ph_min=4.0,ph_max=6.5,color={0.55,0.30,0.20}},
 {name="Boron",ph_min=4.5,ph_max=8.5,color={0.50,0.40,0.20}},
 {name="Copper",ph_min=4.0,ph_max=7.0,color={0.60,0.30,0.15}},
 {name="Zinc",ph_min=4.0,ph_max=7.0,color={0.50,0.35,0.25}}}
local DEF_C={
 macro={label="Macronutrients (N, P, K, S, Ca, Mg)",center=6.7,spread=1.60,color={0.13,0.35,0.20,1}},
 micro={label="Micronutrients (Fe, Mn, B, Cu, Zn)",center=5.7,spread=1.80,color={0.55,0.25,0.08,1}}}

-- data model: single source of truth for both tabs
local function deepcopy(t)if type(t)~="table"then return t end local c={}for k,v in pairs(t)do c[deepcopy(k)]=deepcopy(v)end return c end
local function clamp(v,lo,hi)if v<lo then return lo end if v>hi then return hi end return v end
local nutrients,curves=deepcopy(DEF_N),deepcopy(DEF_C)
local show_grid,show_curves=true,true
local undo_stack,redo_stack={},{}
local status_msg="Chart tab: drag the sliders; the plot redraws live."
local function status(m)status_msg=m end
local function normalize_model()
 for _,n in ipairs(nutrients)do
  if type(n.ph_min)~="number"then n.ph_min=PH_MIN end
  if type(n.ph_max)~="number"then n.ph_max=PH_MAX end
  if type(n.name)~="string"then n.name=tostring(n.name) end
  n.ph_min,n.ph_max=clamp(n.ph_min,PH_MIN,PH_MAX),clamp(n.ph_max,PH_MIN,PH_MAX)
  if n.ph_min>n.ph_max then n.ph_min,n.ph_max=n.ph_max,n.ph_min end
  n.color=type(n.color)=="table"and n.color or {0.5,0.5,0.5}
  for ch=1,3 do n.color[ch]=clamp(tonumber(n.color[ch])or 0.5,0,1)end end
 for _,key in ipairs{"macro","micro"}do
  local c=curves[key]
  if type(c.label)~="string"then c.label=key end
  c.center=clamp(tonumber(c.center)or 6.5,0,14)
  c.spread=clamp(tonumber(c.spread)or 1.5,0.1,5)
  c.color=type(c.color)=="table"and c.color or {0.5,0.5,0.5,1}
  for ch=1,4 do c.color[ch]=clamp(tonumber(c.color[ch])or 0.5,0,1)end end end
local function push_undo()
 table.insert(undo_stack,{nutrients=deepcopy(nutrients),curves=deepcopy(curves)})redo_stack={}end
local function restore()
 nutrients,curves=deepcopy(DEF_N),deepcopy(DEF_C)
 undo_stack,redo_stack={},{}
 status("Reset to default chart.")end

-- text plot (the BUI canvas, in monospace glyphs) + the availability line chart
local function col(ph)return clamp(math.floor((ph-PH_MIN)/(PH_MAX-PH_MIN)*W+0.5),0,W)end
local function band(a,b)
 local i,j=col(a),clamp(math.max(col(b)+1,col(a)+1),1,W)
 return string.rep(" ",i)..string.rep("█",j-i)end
local function gauss(c)                                   -- one availability value per pH sample
 local v={}for k=1,NPTS do
  local d=(PH_MIN+(k-1)/(NPTS-1)*(PH_MAX-PH_MIN)-c.center)/c.spread
  v[k]=math.exp(-0.5*d*d)end
 return v end
local function axis(marks)
 local t={}for k=1,W do t[k]=" "end
 for _,m in ipairs(marks)do local x=col(m[1])+1
  for k=1,#m[2]do if t[x+k-1]then t[x+k-1]=m[2]:sub(k,k)end end end
 return table.concat(t)end
local function gridline()
 local t={}for p=PH_MIN,PH_MAX,0.5 do t[col(p)+1]=p%1==0 and "|"or "·"end
 for k=1,W do if not t[k]then t[k]=" "end end
 return table.concat(t)end

-- data tree (the BUI tree widget as an expandable button list)
local rows,meta,row_n
local function addrow(d,key,kind,disp,cont,k,ni,path)
 row_n=row_n+1
 local r={id=row_n,d=d,dk=key,kind=kind,disp=disp,container=cont,k=k,ni=ni,path=path}
 rows[#rows+1],meta[row_n]=r,r end
local function build_rows(open)
 rows,meta,row_n={},{},0
 addrow(0,"nutrients","group",#nutrients.." items",nil,nil,nil,"N")
 if open.N then
  for i,n in ipairs(nutrients)do
   local ip="N["..i.."]"
   addrow(1,"["..i.."]","group",n.name,nil,nil,i,ip)
   if open[ip]then
    addrow(2,"name","string",string.format("%q",n.name),n,"name",i,ip)
    addrow(2,"ph_min","number",("%.6g"):format(n.ph_min),n,"ph_min",i,ip)
    addrow(2,"ph_max","number",("%.6g"):format(n.ph_max),n,"ph_max",i,ip)
    addrow(2,"color","group","3 items",nil,nil,i,ip.."c")
    if open[ip.."c"]then for ch=1,3 do
     addrow(3,"["..ch.."]","number",("%.6g"):format(n.color[ch]),n.color,ch,i,ip)end end end end end
 addrow(0,"curves","group","2 items",nil,nil,nil,"C")
 if open.C then
  for _,cn in ipairs{"macro","micro"}do
   local c,ip=curves[cn],"C."..cn
   addrow(1,cn,"group","4 items",nil,nil,nil,ip)
   if open[ip]then
    addrow(2,"label","string",string.format("%q",c.label),c,"label",nil,ip)
    addrow(2,"center","number",("%.6g"):format(c.center),c,"center",nil,ip)
    addrow(2,"spread","number",("%.6g"):format(c.spread),c,"spread",nil,ip)
    addrow(2,"color","group","4 items",nil,nil,nil,ip.."c")
    if open[ip.."c"]then for ch=1,4 do
     addrow(3,"["..ch.."]","number",("%.6g"):format(c.color[ch]),c.color,ch,nil,ip)end end end end end
 end

-- widget
local function hslider(id,label,v,lo,hi,step,cb)
 ui.row({gap=8,align="center"},function()
  ui.text(label,{width=COL,nowrap=true})
  cb(ui.slider(id,v,{min=lo,max=hi,step=step,show_value=true,format="%.2f",width="fill"}))end)end
local function phrow(id,n,other,is_min)
 ui.row({gap=8,align="center"},function()
  ui.text(n.name,{width=COL,nowrap=true})
  local v=ui.slider(id,n[other],{min=PH_MIN,max=PH_MAX,step=0.1,show_value=true,format="%.1f",width="fill"})
  if v~=n[other]then
   n[other]=is_min and clamp(v,PH_MIN,n.ph_max)or clamp(v,n.ph_min,PH_MAX)
   status(("%s: %.1f - %.1f"):format(n.name,n.ph_min,n.ph_max))end end)end
local function curve_sl(k,field,lo,hi)
 local c=curves[k]
 hslider(k..field,c.label.." "..field,c[field],lo,hi,0.05,function(v)
  if v~=c[field]then c[field]=v status(("%s %s = %.2f"):format(c.label,field,v))end end)end

local function chart_tab(ui)
 normalize_model()
 ui.card({id="plot",gap=2},function()
  local blank=function()ui.text("",{font="mono",size=11,width=COL})end
  if show_grid then ui.row({gap=8},function()blank()ui.text(gridline(),{font="mono",size=11})end)end
  ui.row({gap=8},function()blank()
   ui.text(axis({{4.4,"ACIDIC"},{6.6,"NEUTRAL"},{8.7,"ALKALINE"}}),{font="mono",size=11})end)
  for _,n in ipairs(nutrients)do
   ui.row({gap=8,align="center"},function()
    ui.text(n.name,{font="mono",size=11,width=COL,nowrap=true})
    ui.text(band(n.ph_min,n.ph_max),{font="mono",size=11})end)end
  if show_curves then
   ui.line_chart("avail",{
    {name=curves.macro.label,values=gauss(curves.macro),color=curves.macro.color},
    {name=curves.micro.label,values=gauss(curves.micro),color=curves.micro.color}},
    {height=220,smooth=true,min=0,max=1,format="%.2f",no_grid=not show_grid,
     labels={"4.0","5.0","6.0","7.0","8.0","9.0","10.0"}})
   ui.row({gap=8},function()blank()ui.text(" pH",{font="mono",size=11})end)
  else
   ui.row({gap=8},function()blank()
    ui.text(axis({{4,"4"},{5,"5"},{6,"6"},{7,"7"},{8,"8"},{9,"9"},{10,"10"}}),{font="mono",size=11})
    ui.text(" pH",{font="mono",size=11})end)end
  ui.secondary("band = available pH range · chart = availability curve (higher = more available)")end)
 ui.card({id="ctrl",gap=4},function()
  ui.label("Nutrient pH ranges")
  for i,n in ipairs(nutrients)do
   phrow("pmin"..i,n,"ph_min",true)phrow("pmax"..i,n,"ph_max",false)end
  ui.separator()ui.label("Availability curves")
  curve_sl("macro","center",5.0,8.5)curve_sl("macro","spread",0.6,3.0)
  curve_sl("micro","center",4.0,7.5)curve_sl("micro","spread",0.6,3.0)
  ui.row({gap=16,align="center"},function()
   show_grid=ui.switch("Grid",show_grid)
   show_curves=ui.switch("Curves",show_curves)
   ui.spacer()
   if ui.button("Reset defaults",{variant="outline"})then restore()end end)end)end

local function table_tab(ui,self)
 normalize_model()
 build_rows(self.open)
 local m=self.sel and meta[self.sel]
 local leaf=m and m.kind~="group"and m.container
 local function apply()
  if not leaf then return end
  local v
  if m.kind=="string"then v=self.ed else
   v=tonumber(self.ed)
   if not v then status("Value must be a number")return end end
  push_undo()m.container[m.k]=v
  status(("Applied %s = %s"):format(m.dk,tostring(v)))end
 local function revert()self.sel,self.ed=nil,""end
 local function swap(from,to)
  if #from==0 then return end
  table.insert(to,{nutrients=deepcopy(nutrients),curves=deepcopy(curves)})
  local s=table.remove(from)
  nutrients,curves=s.nutrients,s.curves
  revert()end
 ui.row({gap=12,align="start"},function()
  ui.column({gap=2,width="fill"},function()
   ui.secondary("expand nutrients/curves, select a leaf, Apply")
   for _,r in ipairs(rows)do
    local lbl=string.rep("   ",r.d)..
     (r.kind=="group"and (self.open[r.path]and "v "or "> ")or "  ")..
     r.dk.."    "..(r.kind=="group"and "table"or r.kind).."    "..r.disp
    if ui.button(lbl,{variant="ghost",full_width=true,selected=self.sel==r.id})then
     if r.kind=="group"then self.open[r.path]=not self.open[r.path]
      self.sel=self.sel==r.id and nil or r.id self.ed=""
     else self.sel,self.ed=r.id,r.kind=="string"and tostring(r.container[r.k])
      or ("%.6g"):format(tonumber(r.container[r.k])or 0)end end end end)
  ui.column({gap=6,width=250},function()
   ui.label("Edit entry")
   ui.secondary("Key: "..(m and m.dk or "-"))
   ui.secondary("Type: "..(m and (m.kind=="group"and "table"or m.kind)or "-"))
   local v,sub=ui.textfield("ed",self.ed or "",{width="fill",placeholder="value",read_only=not leaf})
   self.ed=v
   if sub then apply()end
   if ui.button("Apply",{variant="primary",full_width=true,disabled=not leaf})then apply()end
   if ui.button("+ Nutrient",{variant="outline",full_width=true})then
    push_undo()
    nutrients[#nutrients+1]={name="New Nutrient",ph_min=5.0,ph_max=8.0,color={0.40,0.50,0.40}}
    status("Added nutrient ("..#nutrients.." total)")end
   if ui.button("Delete Nutrient",{variant="danger",full_width=true,disabled=not(m and m.ni)})then
    if not(m and m.ni and nutrients[m.ni])then
     status("Select a nutrient row (or a field inside one) to delete")
    else
     push_undo()
     local nm=nutrients[m.ni].name
     table.remove(nutrients,m.ni)
     revert()
     status("Deleted "..nm.." ("..#nutrients.." left)")end end
   ui.row({gap=6},function()
    if ui.button("Undo",{variant="secondary",width="fill",disabled=#undo_stack==0})then
     swap(undo_stack,redo_stack)status("Undo")end
    if ui.button("Redo",{variant="secondary",width="fill",disabled=#redo_stack==0})then
     swap(redo_stack,undo_stack)status("Redo")end end)
   if ui.button("Reset defaults",{variant="outline",full_width=true})then restore()revert()end end)end)end

widget(function(ui,self)
 self.tab,self.open=self.tab or "chart",self.open or {N=true,C=true}
 ui.card({gap=8},function()
  ui.row({gap=10,align="center"},function()
   ui.heading("Soil pH and Nutrient Availability",3)
   ui.spacer()
   if ui.button("Chart",{variant=self.tab=="chart"and "primary"or "ghost"})then
    self.tab="chart"status("Chart tab: drag the sliders; the plot redraws live.")end
   if ui.button("Table",{variant=self.tab=="table"and "primary"or "ghost"})then
    self.tab="table"status("Table tab: expand nutrients/curves, select a leaf, Apply.")end end)
  ui.muted(status_msg)end)
 if self.tab=="chart"then chart_tab(ui)else table_tab(ui,self)end end)

print("Soil pH and Nutrient Availability")
print("Chart tab: text plot + sliders | Table tab: edit the same data as a\nLua table (expandable rows, Apply, add/delete, undo/redo).\n")
