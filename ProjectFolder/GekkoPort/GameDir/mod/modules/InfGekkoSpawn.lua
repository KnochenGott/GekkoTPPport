-- InfGekkoSpawn.lua 29/09/2026. IH menu "GekkoSpawn": step-by-step spawn/animate of the Survive gekko via the Gekko DLL lib.
-- Menu pattern from IH InfCameraPlayCam.lua. The "InfMenuDefs.inMissionMenu" parentRef is what shows it during missions.
local this={}

-- IH InfCore.Log(message,announceLog,forceLog) drops the line unless IH debugMode is on or forceLog is true
-- (read: IH InfCore.lua). Every log here passes forceLog so it lands in ih_log regardless of IH settings.
local function ForceLog(msg,announce)
  InfCore.Log(msg,announce or false,true)
end

local STEP_NAMES={[0]="None","FilesFound","Created","SetUpDone","MtarAdded","Placed"}

-- Gekko.dll is loaded once with package.loadlib, the same way material_probe.dll is. IH's plugin loader may already
-- have loaded it from its own folder, so every candidate folder is tried and logged,
-- again on each use until one works.
local function DllCandidates()
  local mod=string.gsub(InfCore.paths.mod,"/","\\")
  local game=string.gsub(InfCore.gamePath or "","/","\\")
  if game~="" and string.sub(game,-1)~="\\" then game=game.."\\" end
  return {
    game.."plugins\\Gekko.dll",--IH plugin folder: same file IH already loaded, so one copy in memory
    mod.."plugins\\Gekko.dll",
    game.."mod\\plugins\\Gekko.dll",
    game.."Gekko.dll",
  }
end
local function Lib()
  if Gekko==nil then
    for i,path in ipairs(DllCandidates())do
      local open,err=package.loadlib(path,"luaopen_Gekko")
      ForceLog("InfGekkoSpawn: package.loadlib "..path.." -> "..(open and "ok" or tostring(err)))
      if open then
        local ok,res=pcall(open)
        ForceLog("InfGekkoSpawn: luaopen_Gekko -> "..tostring(ok).." "..tostring(res).." (details in MGS_TPP\\Gekko.log)")
        break
      end
    end
  end
  return Gekko
end

local function DebugOn() return Ivars.gekkoDebugLevel:Get()>0 end

local function Log(msg)
  if not DebugOn() then return end
  ForceLog("InfGekkoSpawn: "..msg)
  local g=Lib()
  if g then g.Log(msg) end
end

-- Runs one lib call, logs args/result, shows result on screen. Everything in pcall so a Lua error never stops the menu.
local function Call(name,...)
  local g=Lib()
  if g==nil then
    InfCore.Log("InfGekkoSpawn: Gekko lib missing, see ih_log and MGS_TPP\\Gekko.log",true,true)
    return false
  end
  local args={...}
  for i=1,#args do args[i]=tostring(args[i]) end
  Log("call "..name.."("..table.concat(args,",")..")")
  local okCall,ok,err=pcall(g[name],...)
  if not okCall then
    InfCore.Log("InfGekkoSpawn: lua error in "..name..": "..tostring(ok),true,true)
    return false
  end
  local msg=name.." -> "..tostring(ok)..((err and err~="") and (" : "..err) or "")
  Log(msg)
  TppUiCommand.AnnounceLogView(msg)
  return ok
end

-- vars.playerRotY is in degrees (measured: Gekko.log showed yaw -68 and -156); the driver builds its matrix in radians.
local function PlayerPose()
  local d=Ivars.gekkoSpawnDistance:Get()
  local yaw=math.rad(vars.playerRotY or 0)
  local x=vars.playerPosX+math.sin(yaw)*d
  local z=vars.playerPosZ+math.cos(yaw)*d
  return x,vars.playerPosY,z,yaw
end

this.spawned=false

this.registerIvars={
  "gekkoLoadPack",
  "gekkoAnimSlot",
  "gekkoActionId",
  "gekkoBlend",
  "gekkoDebugLevel",
  "gekkoSpawnDistance",
  "gekkoDebugNavCtl",
  "gekkoLegIk",
  "gekkoNavStep",
  "gekkoSteerGate",
  "gekkoRealNpc",
  "gekkoDebugFlag",
  "gekkoDebugStateNum",
  "gekkoDamageAmount",
  "gekkoDamagePart",
}

-- Adds the gekko files pack to every mission load. The DLL can only find files the game has loaded.
-- 25/09/2026 a pack with no fpkd hung the load; the pack now has an fpkd with gekko_dll.fox2.
this.gekkoLoadPack={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  settings={"OFF","ON"},
  default=1,
}


this.gekkoAnimSlot={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=0,
  range={max=78,min=0,increment=1},
}
-- Survive action index (db/gecko_actions.tsv), requested through the lifted brain's mailbox.
this.gekkoActionId={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=1,
  range={max=31,min=0,increment=1},
}
-- DEBUG options and part tests. Nav controller: Survive's lifted NavigationController2Impl as nav service, default ON (without it the path is a straight line and nothing is avoided), read when the brain starts.
this.gekkoDebugNavCtl={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=1,
  range={max=1,min=0,increment=1},
  settingNames="set_switch",
  OnChange=function(self,setting)
    local g=Lib()
    if g then g.DebugNavController(setting) end
  end,
}
-- Leg fit (feet follow the ground), default OFF: an animation plugin that lifts or lowers each ANIMAL_LEG foot target to the ground under it (docs/DOCUMENTATION.md 5.12). Not tested in game yet; log lines start with [legik].
this.gekkoLegIk={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=0,
  range={max=1,min=0,increment=1},
  settingNames="set_switch",
  OnChange=function(self,setting)
    local g=Lib()
    if g and g.LegIk then g.LegIk(setting) end
  end,
}
-- Walk path correction, default OFF: Survive's locomotion adds a step toward the nav path to the root motion while it walks (docs/DOCUMENTATION.md 5.13). Not tested in game yet; log lines start with [navstep].
-- Heading request gate, default ON: the walk state's heading request is applied only while the walk state is current (a stale request slid the body after turn clips; docs/DOCUMENTATION.md 4.13).
this.gekkoSteerGate={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=1,
  range={max=1,min=0,increment=1},
  settingNames="set_switch",
  OnChange=function(self,setting)
    local g=Lib()
    if g and g.SteerGate then g.SteerGate(setting) end
  end,
}
-- Real SsdNpc service, default OFF (docs/DOCUMENTATION.md 5.16a): Survive's own NPC object and service slots replace the host-built service. Takes effect at the next spawn. Log lines start with [realnpc].
this.gekkoRealNpc={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=0,
  range={max=1,min=0,increment=1},
  settingNames="set_switch",
  OnChange=function(self,setting)
    local g=Lib()
    if g and g.RealNpc then g.RealNpc(setting) end
  end,
}
this.gekkoNavStep={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=0,
  range={max=1,min=0,increment=1},
  settingNames="set_switch",
  OnChange=function(self,setting)
    local g=Lib()
    if g and g.NavStep then g.NavStep(setting) end
  end,
}
this.gekkoDebugFlag={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=0,
  range={max=15,min=0,increment=1},
}
this.gekkoDebugStateNum={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=12,
  range={max=12,min=0,increment=1},
}
-- TPP damage points of one DEBUG hit and the hit sphere it lands on (GeckoTargetDefense.tgt order: 0 Waist, 1 Head, 2 Head_Nose, 3 Waist_B, 4 Leg_L, 5 Leg_R, 6 Thigh_L, 7 Thigh_R, 8 Knee_L,
-- 9 Knee_R, 10 Shin_L, 11 Shin_R, 12 Camera_Top, 13 Weak_Head, 14 Head_R, 15 Head_L, 16 Head_B, 17 GunR, 18 LocketL, 19 GasL, 20 GasR).
this.gekkoDamageAmount={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=1000,
  range={max=65535,min=10,increment=10},
}
this.gekkoDamagePart={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=0,
  range={max=20,min=0,increment=1},
}
this.gekkoBlend={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=0.2,
  range={max=2,min=0,increment=0.1},
}
-- 0 errors only, 1 every step, 2 +pointers/structs, 3 +every frame (log grows fast)
this.gekkoDebugLevel={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=2,
  range={max=3,min=0,increment=1},
  OnChange=function(self,setting)
    local g=Lib()
    if g then g.SetDebug(setting) end
  end,
}
this.gekkoSpawnDistance={
  inMission=true,
  save=IvarProc.CATEGORY_EXTERNAL,
  default=5,
  range={max=50,min=0,increment=1},
}


-- Main menu: the everyday tests (spawn, despawn, AI and brain start and stop, one action request, status). The rest sits in submenus below it.
this.registerMenus={"gekkoSpawnMenu","gekkoSwitchMenu","gekkoDamageMenu","gekkoAnimMenu","gekkoDebugMenu","gekkoSetupMenu"}

this.gekkoSpawnMenu={
  parentRefs={"InfMenuDefs.safeSpaceMenu","InfMenuDefs.inMissionMenu"},
  options={
    "InfGekkoSpawn.GekkoSpawnAtPlayer",
    "InfGekkoSpawn.GekkoDespawn",
    "InfGekkoSpawn.GekkoAiStart",
    "InfGekkoSpawn.GekkoAiStop",
    "InfGekkoSpawn.GekkoBrainStart",
    "InfGekkoSpawn.GekkoBrainStop",
    "Ivars.gekkoActionId",
    "InfGekkoSpawn.GekkoRequestAction",
    "InfGekkoSpawn.GekkoRouteFollow",
    "InfGekkoSpawn.GekkoBrainStatus",
    "InfGekkoSpawn.GekkoStatus",
    "Ivars.gekkoSpawnDistance",
  },
}

-- Port switches: what the brain runs on and which movement helpers are on. Read when the brain starts (the real service at the next spawn).
this.gekkoSwitchMenu={
  parentRefs={"InfGekkoSpawn.gekkoSpawnMenu"},
  options={
    "Ivars.gekkoRealNpc",
    "Ivars.gekkoDebugNavCtl",
    "Ivars.gekkoSteerGate",
    "Ivars.gekkoNavStep",
    "Ivars.gekkoLegIk",
    "InfGekkoSpawn.GekkoLegIkStatus",
  },
}

-- Hit and damage tests: one debug hit on a chosen hit sphere, the impact effect, and dumps of the hit targets.
this.gekkoDamageMenu={
  parentRefs={"InfGekkoSpawn.gekkoSpawnMenu"},
  options={
    "Ivars.gekkoDamageAmount",
    "Ivars.gekkoDamagePart",
    "InfGekkoSpawn.GekkoDebugDamage",
    "InfGekkoSpawn.GekkoDebugImpact",
    "InfGekkoSpawn.GekkoDumpTargets",
    "InfGekkoSpawn.GekkoPushProbe",
  },
}

-- Animation: play a clip slot of the gekko motion pack directly.
this.gekkoAnimMenu={
  parentRefs={"InfGekkoSpawn.gekkoSpawnMenu"},
  options={
    "Ivars.gekkoAnimSlot",
    "InfGekkoSpawn.GekkoPlaySlot",
    "InfGekkoSpawn.GekkoNextSlot",
    "InfGekkoSpawn.GekkoPrevSlot",
    "Ivars.gekkoBlend",
  },
}

-- Brain debugging: fire single actions, AI states, the chooser, a dispatch, the melee chooser, and write a snapshot to the log.
this.gekkoDebugMenu={
  parentRefs={"InfGekkoSpawn.gekkoSpawnMenu"},
  options={
    "Ivars.gekkoDebugFlag",
    "InfGekkoSpawn.GekkoDebugAction",
    "Ivars.gekkoDebugStateNum",
    "InfGekkoSpawn.GekkoDebugState",
    "InfGekkoSpawn.GekkoDebugChoose",
    "InfGekkoSpawn.GekkoDebugDispatch",
    "InfGekkoSpawn.GekkoDebugMelee",
    "InfGekkoSpawn.GekkoDebugSnapshot",
  },
}

-- Setup: address check, log level and whether the pack is added to mission loads.
this.gekkoSetupMenu={
  parentRefs={"InfGekkoSpawn.gekkoSpawnMenu"},
  options={
    "InfGekkoSpawn.GekkoVerify",
    "Ivars.gekkoDebugLevel",
    "Ivars.gekkoLoadPack",
  },
}

this.GekkoVerify=function()
  local g=Lib()
  if g then g.SetDebug(Ivars.gekkoDebugLevel:Get()) end
  Call("Verify")
end
local function SyncNavOption(g)
  if g and g.DebugNavController then g.DebugNavController(Ivars.gekkoDebugNavCtl:Get()) end
  if g and g.LegIk then g.LegIk(Ivars.gekkoLegIk:Get()) end
  if g and g.NavStep then g.NavStep(Ivars.gekkoNavStep:Get()) end
  if g and g.SteerGate then g.SteerGate(Ivars.gekkoSteerGate:Get()) end
  if g and g.RealNpc then g.RealNpc(Ivars.gekkoRealNpc:Get()) end
end
this.GekkoLegIkStatus=function()
  local g=Lib()
  if g and g.LegIkStatus then InfCore.Log("InfGekkoSpawn: leg ik "..g.LegIkStatus(),true,true) end
end
this.GekkoDebugAction=function()
  local g=Lib()
  if g==nil then return end
  g.DebugAction(Ivars.gekkoActionId:Get(),Ivars.gekkoDebugFlag:Get(),vars.playerPosX,vars.playerPosY,vars.playerPosZ)
end
this.GekkoDebugState=function() local g=Lib() if g then g.DebugState(Ivars.gekkoDebugStateNum:Get()) end end
this.GekkoDebugChoose=function() local g=Lib() if g then g.DebugChoose() end end
this.GekkoDebugDispatch=function() local g=Lib() if g then g.DebugDispatch() end end
this.GekkoDebugMelee=function() local g=Lib() if g then g.DebugMelee(vars.playerPosX,vars.playerPosY,vars.playerPosZ) end end
this.snapCount=0
this.GekkoDebugSnapshot=function()
  local g=Lib()
  if g==nil then return end
  this.snapCount=this.snapCount+1
  g.DebugSnapshot("mark "..this.snapCount)
  InfCore.Log("InfGekkoSpawn: snapshot "..this.snapCount.." written to Gekko.log ([snap])",true,true)
end
this.GekkoDebugDamage=function()
  local g=Lib()
  if g and g.DebugDamage then g.DebugDamage(Ivars.gekkoDamageAmount:Get(),Ivars.gekkoDamagePart:Get()) end
end
this.GekkoDebugImpact=function()
  local g=Lib()
  if g and g.DebugImpact then g.DebugImpact(Ivars.gekkoDamagePart:Get(),vars.playerPosX,vars.playerPosY,vars.playerPosZ) end
end
this.GekkoSpawnAtPlayer=function()
  SyncNavOption(Lib())
  local x,y,z,yaw=PlayerPose()
  --native TppGekko game object (index 100). The pack's fox2 GameObject entity creates it at load without a position:
  --TypePlace spawns that one; with no fox2 object realized, TypeCreate makes one
  this.spawned=Call("TypePlace",x,y,z,yaw) or Call("TypeCreate",x,y,z,yaw)
end
this.GekkoPlaySlot=function() Call("Play",Ivars.gekkoAnimSlot:Get(),Ivars.gekkoBlend:Get()) end
this.GekkoNextSlot=function()
  Ivars.gekkoAnimSlot:Set((Ivars.gekkoAnimSlot:Get()+1)%79)
  this.GekkoPlaySlot()
end
this.GekkoPrevSlot=function()
  Ivars.gekkoAnimSlot:Set((Ivars.gekkoAnimSlot:Get()+78)%79)
  this.GekkoPlaySlot()
end
this.aiOn=false
this.GekkoAiStart=function()
  local g=Lib()
  if g==nil then return Call("AiStart") end
  SyncNavOption(g)
  g.AiStart()
  this.aiOn=true
  ForceLog("InfGekkoSpawn: Ai started",true)
end
this.GekkoAiStop=function()
  local g=Lib()
  if g then g.AiStop() end
  this.aiOn=false
  ForceLog("InfGekkoSpawn: Ai stopped",true)
end
this.GekkoBrainStart=function()
  local g=Lib()
  if g==nil then return end
  SyncNavOption(g)
  g.BrainStart()
  this.aiOn=true
  ForceLog("InfGekkoSpawn: Survive brain started",true)
end
this.GekkoBrainStop=function()
  local g=Lib()
  if g then g.BrainStop() end
  this.aiOn=false
  ForceLog("InfGekkoSpawn: Survive brain stopped",true)
end
this.GekkoRequestAction=function()
  local g=Lib()
  if g==nil then return end
  g.BrainRequest(Ivars.gekkoActionId:Get(),0,vars.playerPosX,vars.playerPosY,vars.playerPosZ)
end
this.GekkoBrainStatus=function()
  local g=Lib()
  if g==nil then return end
  InfCore.Log("InfGekkoSpawn: "..tostring(g.BrainStatus()),true,true)
end
this.GekkoDumpTargets=function()
  local g=Lib()
  if g then g.DumpTargets() end
  InfCore.Log("InfGekkoSpawn: target objects dumped to Gekko.log ([tprobe])",true,true)
end
this.GekkoPushProbe=function()
  local g=Lib()
  if g then g.PushProbe(1.5,1) g.PushProbe(1.5,0) end
  InfCore.Log("InfGekkoSpawn: push probe written to Gekko.log ([pushprobe])",true,true)
end
-- Debug: follow the nearest TPP route. Routes come from InfGekkoRoutes.lua (tools/route_nodes.py --all, every retail .frl rail). The nearest rail is the one with the closest vertex in x/z among
-- vertices within 40 m in height (routes of different worlds overlap in x/z). The gekko then walks vertex to vertex (Survive's movement brain gets each vertex as its target through Gekko.AiTarget),
-- turns around at the ends of an open rail and loops a closed one. Needs the brain started ("Survive brain: start").
this.routeOn=false
local routeData
local function Routes()
  if routeData==nil then
    local path=string.gsub(InfCore.paths.mod,"/","\\").."modules\\InfGekkoRoutes.lua"
    local ok,res=pcall(dofile,path)
    routeData=(ok and type(res)=="table") and res or false
    ForceLog("InfGekkoSpawn: routes "..(routeData and (#routeData.." rails loaded") or ("NOT loaded: "..tostring(res))))
  end
  return routeData
end
local function GekkoPos()
  local g=Gekko
  if g==nil then return nil end
  local x,y,z=string.match(g.GetPose() or "","^(%S+) (%S+) (%S+)")
  if x==nil then return nil end
  return tonumber(x),tonumber(y),tonumber(z)
end
local function PickRoute(x,y,z)
  local routes=Routes()
  if not routes then return nil end
  local best,bi,bn=nil,nil,nil
  for ri,r in ipairs(routes) do
    for ni,n in ipairs(r.nodes) do
      if math.abs(n[2]-y)<40 then
        local dx,dz=n[1]-x,n[3]-z
        local d=dx*dx+dz*dz
        if best==nil or d<best then best,bi,bn=d,ri,ni end
      end
    end
  end
  if best==nil then return nil end
  return bi,bn,math.sqrt(best)
end
this.GekkoRouteFollow=function()
  if this.routeOn then
    this.routeOn=false
    this.route=nil
    ForceLog("InfGekkoSpawn: route following off",true)
    return
  end
  if not this.spawned or not this.aiOn then
    ForceLog("InfGekkoSpawn: spawn the gekko and start the Survive brain first",true)
    return
  end
  local x,y,z=GekkoPos()
  if x==nil then ForceLog("InfGekkoSpawn: no gekko pose",true) return end
  local ri,ni,d=PickRoute(x,y,z)
  if ri==nil then ForceLog("InfGekkoSpawn: no route within 40 m height of the gekko",true) return end
  local r=routeData[ri]
  this.route={rail=r,idx=ni,dir=1}
  this.routeOn=true
  ForceLog(string.format("InfGekkoSpawn: following %s rail %d (%d nodes), nearest node %d at %.1f m",r.name,r.rail,#r.nodes,ni,d),true)
end
local function RouteTarget()
  local st=this.route
  if not st then return nil end
  local x,y,z=GekkoPos()
  if x==nil then return nil end
  local nodes=st.rail.nodes
  local n=nodes[st.idx]
  local dx,dz=n[1]-x,n[3]-z
  if dx*dx+dz*dz<9.0 then
    local closed=#nodes>2 and (nodes[1][1]-nodes[#nodes][1])^2+(nodes[1][3]-nodes[#nodes][3])^2<25.0
    local nxt=st.idx+st.dir
    if nxt>#nodes then if closed then nxt=1 else st.dir=-1 nxt=#nodes-1 end
    elseif nxt<1 then if closed then nxt=#nodes else st.dir=1 nxt=2 end end
    if nxt<1 then nxt=1 end
    st.idx=nxt
    n=nodes[st.idx]
    ForceLog(string.format("InfGekkoSpawn: route node %d/%d",st.idx,#nodes))
  end
  return n[1],n[2],n[3]
end

this.GekkoDespawn=function()
  if this.aiOn then this.GekkoAiStop() end
  local g=Lib()
  if g then g.TypeDestroy() end
  this.spawned=false
end
this.GekkoStatus=function()
  local g=Lib()
  if g==nil then return Call("Status") end
  local step,verified,err=g.Status()
  local msg="step "..tostring(STEP_NAMES[step] or step).." verified "..tostring(verified).." err "..tostring(err)
  InfCore.Log("InfGekkoSpawn: "..msg,true,true)
end

local GEKKO_PACK="/Assets/tpp/pack/gekko/gekko_dll.fpk"

-- Gameplay missions only (not title, not 4xxxx heli/MB space).
local function IsGameplayMission(c)
  return c and c>=10000 and c~=65535 and not (c>=40000 and c<50000)
end

-- Called by InfMain.AddMissionPacks while the mission's pack list is built; takes effect on the next mission load.
function this.AddMissionPacks(missionCode,packPaths)
  ForceLog("InfGekkoSpawn.AddMissionPacks "..tostring(missionCode).." loadPack="..tostring(Ivars.gekkoLoadPack:Get()))
  if Ivars.gekkoLoadPack:Get()==0 or not IsGameplayMission(missionCode) then return end
  --the fox2 GameObject entity (typeName TppGekko) needs the type registered before the pack loads
  Call("TypeRegister")
  packPaths[#packPaths+1]=GEKKO_PACK
  ForceLog("InfGekkoSpawn.AddMissionPacks "..tostring(missionCode)..": added "..GEKKO_PACK)
  if Ivars.gekkoDebugLevel:Get()>=2 then
    for i,path in ipairs(packPaths)do
      ForceLog("InfGekkoSpawn.AddMissionPacks   ["..i.."] "..tostring(path))
    end
  end
end


-- Mission lifecycle, logged so ih_log shows which mission the pack list was built for.
function this.OnAllocate(missionTable)
  ForceLog("InfGekkoSpawn.OnAllocate mission "..tostring(vars and vars.missionCode))
end
function this.Init(missionTable)
  ForceLog("InfGekkoSpawn.Init mission "..tostring(vars and vars.missionCode))
end

-- The gekko ticks itself in the DLL (TppGekko ExecuteSerially). Lua only feeds the player position to the AI
-- until the DLL reads it natively.
function this.Update(currentChecks,currentTime,execChecks,execState)
  if not this.spawned or not this.aiOn then return end
  local g=Gekko
  if g==nil then return end
  if this.routeOn then
    local rx,ry,rz=RouteTarget()
    if rx then pcall(g.AiTarget,rx,ry,rz) end
  else
    pcall(g.AiTarget,vars.playerPosX,vars.playerPosY,vars.playerPosZ)
  end
  if DebugOn() then
    local st=g.AiState()
    if st~=this.lastAiState then Log("ai state "..tostring(st)) this.lastAiState=st end
  end
end

this.langStrings={
  eng={
    gekkoSpawnMenu="GekkoSpawn",
    gekkoSwitchMenu="Port switches",
    gekkoDamageMenu="Hits and damage",
    gekkoAnimMenu="Animation",
    gekkoDebugMenu="Brain debugging",
    gekkoSetupMenu="Setup and logging",
    gekkoVerify="Verify exe (run first)",
    gekkoSpawnAtPlayer="Spawn in front of player",
    gekkoAiStart="AI: start (walks to player)",
    gekkoAiStop="AI: stop",
    gekkoBrainStart="Survive brain: start",
    gekkoBrainStop="Survive brain: stop",
    gekkoActionId="Survive action id",
    gekkoRequestAction="Request that action",
    gekkoBrainStatus="Survive brain: log status",
    gekkoDumpTargets="Dump hit targets (gekko vs natives)",
    gekkoPushProbe="Push probe (CharaControl human check through the gekko)",
    gekkoRouteFollow="Follow nearest route (debug, toggle)",
    gekkoPlaySlot="Play anim slot",
    gekkoAnimSlot="Anim slot",
    gekkoNextSlot="Next slot and play",
    gekkoPrevSlot="Prev slot and play",
    gekkoBlend="Blend time",
    gekkoDebugNavCtl="DEBUG: lifted nav controller (off = straight line)",
    gekkoLegIk="Leg fit: feet follow the ground (plugin, not tested)",
    gekkoLegIkStatus="Leg fit: log status",
    gekkoNavStep="Walk path correction (Survive's step toward the path, not tested)",
    gekkoSteerGate="Heading request only while walking (stops the slide after turns)",
    gekkoRealNpc="Real Survive NPC service (next spawn; experimental)",
    gekkoDebugFlag="DEBUG: action flag",
    gekkoDebugAction="DEBUG: fire action id (uses Survive action id above)",
    gekkoDebugStateNum="DEBUG: AI state number 0..12",
    gekkoDebugState="DEBUG: run exit+enter of that AI state",
    gekkoDebugChoose="DEBUG: run Survive state chooser",
    gekkoDebugDispatch="DEBUG: one action dispatch",
    gekkoDebugMelee="DEBUG: Survive melee chooser (target = player)",
    gekkoDebugSnapshot="DEBUG: snapshot to Gekko.log (mark moment)",
    gekkoDamageAmount="DEBUG: damage amount (TPP points)",
    gekkoDamagePart="DEBUG: damage hit sphere 0..20 (see Gekko.log)",
    gekkoDebugDamage="DEBUG: apply that damage",
    gekkoDebugImpact="DEBUG: impact effect on that sphere (no gun, see Gekko.log [hitfx])",
    gekkoDespawn="Despawn",
    gekkoStatus="Log status",
    gekkoSpawnDistance="Spawn distance",
    gekkoDebugLevel="Debug log level",
    gekkoLoadPack="Load gekko pack on mission load",
  },
  help={
    eng={
      gekkoVerify="Checks every address signature against the exe. Result in MGS_TPP\\Gekko.log.",
      gekkoDebugLevel="0 errors, 1 every step, 2 +pointers and structs, 3 +every frame. Log: MGS_TPP\\Gekko.log and ih_log.",
      gekkoLoadPack="Adds the gekko model and animation pack to the next mission load. Needed for find files to succeed.",
    },
  },
}

return this
