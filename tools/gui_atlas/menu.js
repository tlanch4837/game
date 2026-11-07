// Full GUI menu tree, built from the game code (see docs/gui_inventory.md).
// item: [name, costLabel, icon, status]   status: '' = in game, 'u' = UI unfinished, 'p' = planned slot
// cat.modes = tool modes shown in the placement bar while a tool from this category is active
const MENU=[
 {id:'roads',n:'Roads',i:'road',col:'#7d8597',
  modes:[['Straight','straight'],['Curve','curve'],['Complex','complex'],['Continuous','continuous'],['Grid','grid'],['Replace','replace'],['Swap','swap'],['Bulldoze','bulldoze']],
  opts:[['Elevation','elev'],['Rotate','rotate'],['Snapping','snap'],['Parallel','parallel'],['Underground','under']],
  groups:[
  {n:'Small',i:'road_s',items:[['Gravel Road','$15/m','road_s'],['Gravel One-Way','$15/m','road_s'],['Alley','$20/m','road_s'],['One-Way Alley','$20/m','road_s'],['Angled Parking Alley','$25/m','road_s'],['Perpendicular Parking Alley','$25/m','road_s'],['Two-Lane Road','$40/m','road_s'],['Two-Lane One-Way','$40/m','road_s'],['One-Lane One-Way','$30/m','road_s'],['One-Lane Angled Parking','$35/m','road_s'],['Two-Lane Angled Parking','$50/m','road_s'],['Two-Lane Divided Angled Parking','$55/m','road_s'],['Two-Lane Perpendicular Parking','$50/m','road_s']]},
  {n:'Medium',i:'road_m',items:[['Three-Lane One-Way','$70/m','road_m'],['Three-Lane Asymmetric','$75/m','road_m'],['Four-Lane Road','$90/m','road_m'],['Four-Lane Divided','$100/m','road_m'],['Four-Lane One-Way','$90/m','road_m'],['Four-Lane Asymmetric','$95/m','road_m'],['Four-Lane Angled Parking','$110/m','road_m'],['Four-Lane Perpendicular Parking','$110/m','road_m']]},
  {n:'Large',i:'road_l',items:[['Five-Lane One-Way','$130/m','road_l'],['Five-Lane Asymmetric','$135/m','road_l'],['Six-Lane Road','$150/m','road_l'],['Six-Lane Divided','$165/m','road_l'],['Six-Lane One-Way','$150/m','road_l'],['Six-Lane Asymmetric','$155/m','road_l'],['Seven-Lane One-Way','$175/m','road_l'],['Seven-Lane Asym 4+3','$180/m','road_l'],['Seven-Lane Asym 5+2','$180/m','road_l'],['Eight-Lane Divided','$210/m','road_l']]},
  {n:'Highways',i:'highway',items:[['Two-Lane Highway','$160/m','highway'],['Three-Lane Highway','$200/m','highway'],['Four-Lane Highway','$240/m','highway'],['Two-Lane One-Way Hwy','$140/m','highway'],['Three-Lane One-Way Hwy','$180/m','highway'],['Four-Lane Asym Hwy','$230/m','highway'],['Five-Lane Asym Hwy','$260/m','highway']]},
  {n:'Paths',i:'path',items:[['Pedestrian Path','$8/m','path'],['Bicycle Path','$10/m','bike'],['Pedestrian Street','$30/m','path'],['Small Pedestrian Street','$20/m','path']]},
  {n:'Quays',i:'quay',items:[['Medium Quay','$300/m','quay'],['Wide Quay','$420/m','quay']]},
  {n:'Railways',i:'rail',items:[['Single Track','$120/m','rail'],['Double Track','$200/m','rail']]},
  {n:'Intersections',i:'xing',items:[['Small 4-Way','$1.2k','xing'],['Medium 4-Way','$2.4k','xing'],['Large 4-Way','$4k','xing'],['Small T','$900','t_xing'],['Medium T','$1.8k','t_xing'],['Y-Intersection','$1.6k','t_xing'],['Five-Way','$3.2k','xing']]},
  {n:'Roundabouts',i:'roundabout',items:[['Small Roundabout','$1.8k','roundabout'],['Medium Roundabout','$3k','roundabout'],['Large Roundabout','$5k','roundabout'],['Highway Roundabout','$9k','roundabout']]},
  {n:'Traffic',i:'traffic',items:[['Traffic Lights','','traffic','p'],['Stop Signs','','traffic','p'],['Lane Connections','','swap','p']]}]},
 {id:'zones',n:'Zoning',i:'zone',col:'#4fae5b',
  modes:[['Brush','brush'],['Fill block','bucket'],['Box','box'],['Dezone','dezone']],opts:[['Brush size','brush'],['Zone grid','grid']],
  groups:[
  {n:'Residential',i:'house',items:[['Low Density','','house'],['Row Housing','','row'],['Medium Density','','apartment'],['High Density','','apartment'],['Mixed Use','','shop','p']]},
  {n:'Commercial',i:'shop',items:[['Commercial','','shop']]},
  {n:'Office',i:'office',items:[['Office','','office']]},
  {n:'Industrial',i:'industry',items:[['Industrial','','industry']]}]},
 {id:'terrain',n:'Terrain',i:'terr',col:'#a07a4f',
  modes:[['Raise','raise'],['Dig','dig'],['Flatten','flatten']],opts:[['Brush size','brush'],['Strength','elev'],['Contours','contour']],
  groups:[{n:'Shape',i:'terr',items:[['Raise','','raise'],['Dig','','dig'],['Flatten','','flatten'],['Contours','','contour']]},
  {n:'Nature',i:'tree',items:[['Tree Brush','$20','tree','p'],['Water Source','','water','p']]}]},
 {id:'util',n:'Utilities',i:'util',col:'#f2b11c',opts:[['Rotate','rotate'],['Elevation','elev']],
  groups:[
  {n:'Electricity',i:'power',items:[['Wind Turbine','$12k','wind'],['Solar Power Station','$30k','solar'],['Small Coal Plant','$40k','coal'],['Coal Power Plant','$62k','coal'],['Gas Power Plant','$70k','gas'],['Geothermal Plant','$90k','geo'],['Nuclear Power Plant','$380k','nuclear'],['Emergency Generator','$8k','generator'],['Battery Station','$22k','battery'],['Power Line','$12/m','pylon']]},
  {n:'Water & Sewage',i:'water',items:[['Water Tower','$9k','tower'],['Water Pumping Station','$18k','pump'],['Large Pumping Station','$45k','pump'],['Groundwater Pump','$14k','groundwater'],['Sewage Outlet','$14k','sewage'],['Wastewater Treatment','$60k','treatment'],['Water Pipe','$6/m','pipe'],['Sewage Pipe','$6/m','pipe']]},
  {n:'Garbage',i:'trash',items:[['Landfill','$20k','landfill'],['Incineration Plant','$75k','incinerator'],['Recycling Center','$55k','recycle'],['Industrial Waste Site','$90k','hazard']]}]},
 {id:'serv',n:'Services',i:'serv',col:'#e8465c',opts:[['Rotate','rotate']],
  groups:[
  {n:'Healthcare',i:'health',subs:[{n:'Health',i:'health',items:[['Family Clinic','$24k','health'],['Community Hospital','$80k','health'],['General Hospital','$120k','health'],['Medical Centre','$180k','health'],['Regional Medical Centre','$260k','health']]},{n:'Deathcare',i:'death',items:[['Small Cemetery','$10k','death'],['Memorial Cemetery','$25k','death'],['Crematorium','$40k','death'],['Large Crematorium','$70k','death'],['Crematorium Campus','$120k','death']]}]},
  {n:'Fire & Rescue',i:'fire',items:[['Neighbourhood Firehouse','$18k','fire'],['Fire Station','$30k','fire'],['Modern Fire Station','$45k','fire'],['Regional Fire Station','$80k','fire'],['Fire Headquarters','$140k','fire']]},
  {n:'Police',i:'police',items:[['Police Post','$15k','police'],['Police Station','$32k','police'],['Police Headquarters','$110k','police'],['Police Academy','$90k','police'],['County Jail','$150k','police']]},
  {n:'Education',i:'school',items:[['Rural Elementary','$20k','school'],['Elementary School','$40k','school'],['Rural High School','$45k','school'],['High School','$80k','school'],['College','$210k','school']]},
  {n:'Communications',i:'comms',items:[['Small Antenna','$5k','comms'],['Radio Mast','$12k','comms'],['Cell Tower','$20k','comms'],['Telecom Center','$70k','comms']]},
  {n:'Parks',i:'park',items:[['Small Park','$6k','park','u'],['Plaza','$10k','park','u'],['Sports Field','$25k','park','p']]},
  {n:'Admin',i:'admin',items:[['City Hall','','admin','p'],['Post Office','','admin','p']]}]},
 {id:'transit',n:'Transport',i:'bus',col:'#2f8fe0',
  groups:[
  {n:'Lines',i:'line',items:[['Bus Line','$2k','bus'],['Tram Line','$4k','tram'],['Train Line','$6k','train'],['Subway Line','$8k','subway'],['Boat Line','$5k','ship'],['Plane Line','$10k','plane'],['Cargo Line','$5k','cargo'],['Line Manager','','line','p']]},
  {n:'Road transit',i:'bus',items:[['Bus Depot','$30k','bus'],['Compact Bus Depot','$18k','bus'],['Tram Depot','$45k','tram'],['Compact Tram Depot','$28k','tram'],['Taxi Depot','$20k','taxi'],['Small Taxi Depot','$12k','taxi']]},
  {n:'Rail',i:'train',items:[['Railway Depot','$60k','depot'],['Train Station','$90k','train'],['Modern Station','$130k','train'],['Cargo Terminal','$110k','cargo']]},
  {n:'Ship',i:'ship',items:[['Shipyard','$80k','ship'],['Cargo Harbor','$140k','cargo'],['Passenger Harbor','$120k','ship']]},
  {n:'Air',i:'plane',items:[['Small Airfield','$150k','plane'],['Regional Airport','$400k','plane']]},
  {n:'Connections',i:'outside',items:[['Road Connection','','outside'],['Rail Connection','','outside'],['Sea Connection','','outside'],['Air Connection','','outside']]}]},
 {id:'industry',n:'Industry',i:'factory',col:'#c27c3a',modes:[['Place hub','factory'],['Draw area','area']],
  groups:[{n:'Specialized',i:'factory',items:[['Grain Farm','$35k','grain'],['Livestock Farm','$40k','livestock'],['Forestry','$30k','forestry'],['Oil Drilling','$90k','oil'],['Ore Mining','$80k','ore']]}]},
 {id:'city',n:'City',i:'city',col:'#8b5cf6',panel:true,
  groups:[{n:'Economy',i:'coin',items:[['Budget','','coin'],['Taxes','','taxes'],['Service Funding','','funding'],['Loans','','loan','p']]},
  {n:'Land',i:'tiles',items:[['Map Tiles','','tiles'],['Districts','','district','p'],['Policies','','policy','p']]},
  {n:'Progress',i:'milestone',items:[['Milestones','','milestone']]}]},
 {id:'info',n:'Info',i:'info',col:'#14b8a6',panel:true,
  groups:[{n:'City',i:'stats',items:[['Statistics','','stats','u'],['City Information','','info','u'],['Population','','ppl','u'],['Demand (RCI)','','demand','p']]},
  {n:'People',i:'citizen',items:[['Citizens','','citizen'],['Notifications','','bell','u'],['News & Radio','','radio','u'],['Advisor','','advisor','u']]}]},
];
const VIEWS=[['Power','power','#f2b11c'],['Water','water','#2f8fe0'],['Sewage','sewage','#8a6a3a'],['Fire','fire','#e8465c'],['Police','police','#4f6bed'],['Health','health','#e85a8a'],
 ['Education','school','#9b6bdf'],['Parks','park','#4fae5b'],['Transit','bus','#2f8fe0'],['Demand','demand','#14b8a6'],['Resources','resource','#c27c3a'],['Contours','contour','#a07a4f'],
 ['Underground','under','#7d8597'],['Traffic','traffic','#e14c3a','p'],['Pollution','pollution','#6b7280','p'],['Land Value','landvalue','#22c55e','p'],['Noise','noise','#a855f7','p']];
const CAMERA=[['City camera','fly'],['Walk','walk'],['First person','person'],['Photo mode','cam','p'],['Hide GUI','hide']];
const PAUSE=[['Resume','play'],['Save','save'],['Load','folder'],['New map','newmap'],['Settings','gear'],['Photo mode','cam'],['Main menu','exit']];
