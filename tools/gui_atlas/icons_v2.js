// Icon refinements, loaded after icons.js.
// - redraws the weakest glyphs with cleaner, closed shapes (so they take the duotone fill)
// - marks pure line glyphs as no-fill
Object.assign(ICONS,{
 road:'M8 3h8l4 18H4zM12 6v2m0 4v2m0 4v2',
 trash:'M5 7h14l-1.3 13.1a1 1 0 0 1-1 .9H7.3a1 1 0 0 1-1-.9zM3 7h18M9 7V4.5a.5.5 0 0 1 .5-.5h5a.5.5 0 0 1 .5.5V7M10 11v6m4-6v6',
 bus:'M6 3h12a2 2 0 0 1 2 2v11H4V5a2 2 0 0 1 2-2zM4 11h16M7 16v3m10-3v3M7.5 13.5h.01M16.5 13.5h.01M8 6h8',
 train:'M7 3h10a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2zM5 10h14M8 16l-2 4M16 16l2 4M8.5 13h.01M15.5 13h.01',
 tram:'M7 6h10a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2zM5 12h14M9 3h6M12 3v3M8 18l-2 3M16 18l2 3',
 house:'M4 10.5 12 4l8 6.5V20H4zM10 20v-5h4v5',
 apartment:'M6 3h12v18H6zM9 7h2m2 0h2M9 11h2m2 0h2M9 15h2m2 0h2M10.5 21v-3h3v3',
 office:'M4 8l8-5 8 5v13H4zM8 11h2m4 0h2M8 15h2m4 0h2M10 21v-3h4v3',
 shop:'M4 10h16v10H4zM3 10l2-6h14l2 6zM10 20v-5h4v5M3 10h18',
 industry:'M3 21V11l6 3.5V11l6 3.5V5h4v16zM7 17h2m3 0h2',
 coal:'M3 21V12l5 3v-3l5 3V8h5v13zM14 8V5M17 8V5M14.5 3.5c.6-.6 1.4-.6 2 0',
 wind:'M12 11v10M9 21h6M12 11c-1-3-1-6 0-8 1 2 1 5 0 8zM12 11c2.6-1.6 5.6-2.2 7.8-1-1.8 1.4-4.8 1.6-7.8 1zM12 11c-2.6-1.6-5.6-2.2-7.8-1 1.8 1.4 4.8 1.6 7.8 1z',
 solar:'M4 16 6.5 6h11L20 16zM5.2 11h13.6M10 6l-.8 10M14 6l.8 10M12 16v4M8 20h8',
 tower:'M7 3h10v7a2 2 0 0 1-2 2H9a2 2 0 0 1-2-2zM9 12l-2 9M15 12l2 9M8 17h8',
 pump:'M4 10h9v11H4zM13 14h5V8h3M8.5 10V6h4M4 21h17M8.5 14.5a1.5 1.5 0 1 0 0 3',
 fire:'M12 3c3.5 4 6.5 6.5 6.5 11a6.5 6.5 0 0 1-13 0c0-3 1.7-5 3-6.3.2 2.1 1.2 3.6 2.8 4.1-.8-3.2.2-6 .7-8.8z',
 police:'M12 3l8 3v6c0 5-3.5 8.2-8 9-4.5-.8-8-4-8-9V6zM12 8l1.2 2.4 2.6.4-1.9 1.8.5 2.6L12 14l-2.4 1.2.5-2.6-1.9-1.8 2.6-.4z',
 school:'M2 9.5 12 4l10 5.5L12 15zM6 12v4.5c3.2 2.2 8.8 2.2 12 0V12M21.5 9.5V15',
 death:'M7 21V10a5 5 0 0 1 10 0v11zM4 21h16M12 10v6M9.5 12.5h5',
 health:'M9.5 3h5v6.5H21v5h-6.5V21h-5v-6.5H3v-5h6.5z',
 comms:'M12 11v10M8.5 21h7M12 11l-3.5 10M12 11l3.5 10M8.2 7.2a5.5 5.5 0 0 1 7.6 0M5.4 4.4a9.5 9.5 0 0 1 13.2 0M12 8.6a1.4 1.4 0 1 0 0 2.8 1.4 1.4 0 0 0 0-2.8z',
 park:'M7 3a4 4 0 0 1 4 4c0 2-1.5 3.5-4 3.5S3 9 3 7a4 4 0 0 1 4-4zM7 10.5V21M15 9h6l-3-6zM18 9v12M3 21h18',
 ship:'M4 14h16l-2 6H6zM7 14V9h10v5M11 9V5h3v4M3 21c2 0 2-1 4.5-1S10 21 12 21s2-1 4.5-1 2.5 1 4.5 1',
 taxi:'M5 11l2-5h10l2 5h1a1 1 0 0 1 1 1v4H3v-4a1 1 0 0 1 1-1zM10 3h4v3h-4zM6.5 16v3M17.5 16v3M7 13.5h.01M17 13.5h.01',
 livestock:'M7 7c-1.8 0-3.4-1-4-3 1.8-.4 3.3.3 4.4 1.6M17 7c1.8 0 3.4-1 4-3-1.8-.4-3.3.3-4.4 1.6M7 6h10v8a5 5 0 0 1-10 0zM9.5 10h.01M14.5 10h.01M9.5 16a2.5 1.6 0 0 0 5 0 2.5 1.6 0 0 0-5 0z',
 oil:'M3 21h18M6 21l3-8 3 8M7.2 18h3.6M2.5 8.5 14 4l3.5 2.2L6 10.7zM14 4l2.2 9.5M16.2 13.5V16M17.5 6.2l2 1',
 ore:'M3 9.5c5-5 13-5 18 0M12 6.2V21M9.5 21h5',
 grain:'M12 21V7M12 7c-2.8 0-4-2.5-4-4.5 2.8 0 4 2 4 4.5zM12 7c2.8 0 4-2.5 4-4.5-2.8 0-4 2-4 4.5zM12 12c-2.8 0-4-2.5-4-4.5 2.8 0 4 2 4 4.5zM12 12c2.8 0 4-2.5 4-4.5-2.8 0-4 2-4 4.5zM12 17c-2.8 0-4-2.5-4-4.5 2.8 0 4 2 4 4.5zM12 17c2.8 0 4-2.5 4-4.5-2.8 0-4 2-4 4.5z',
 forestry:'M8 3l4.5 7H10l3.5 5.5h-11L6 10H3.5zM8 15.5V19M13 21h8M14.5 18h5a1.5 1.5 0 0 1 0 3M14.5 18a1.5 1.5 0 0 0 0 3',
 walk:'M13.5 3.5a1.6 1.6 0 1 0 0 3.2 1.6 1.6 0 0 0 0-3.2zM9 21l2.6-6 2.6 2.6V21M7.5 12.5 10 8.5l4 1.4 2 3M10 8.5 8.6 14',
 bell:'M6 16v-5a6 6 0 0 1 12 0v5l1.5 2h-15zM10 20.5a2 2 0 0 0 4 0',
 radio:'M3 9h18v11H3zM7 9l9-5.5M8 12.5a2 2 0 1 0 0 4 2 2 0 0 0 0-4zM14 13h4M14 16h4',
 coin:'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM12 6.5v11M14.8 9.4c-.4-.9-1.5-1.5-2.8-1.5-1.7 0-3 .9-3 2s1.3 1.7 3 2.1 3 1 3 2.1-1.3 2-3 2c-1.4 0-2.5-.6-2.9-1.5',
 stats:'M4 20h16M6 20v-6h3v6M10.5 20V9h3v11M15 20v-9h3v9',
 layers:'M12 3l9.5 5L12 13 2.5 8zM2.5 12.5 12 17.5l9.5-5M2.5 16.5 12 21.5l9.5-5',
 info:'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM12 11v6M12 7.5v.01',
 factory:'M3 21V10l5 3V10l5 3V10l5 3V4h3v17zM7 17h2m3 0h2m3 0h2',
 landfill:'M2 20l5.5-8 4 3 4-6.5L22 20zM14 5.5h4l-1-2.5h-2z',
 recycle:'M7.5 19H4.2a1 1 0 0 1-.9-1.5L5 14.5M16.5 19h3.3a1 1 0 0 0 .9-1.5L19 14.5M10 4.5l1.1-1.9a1 1 0 0 1 1.8 0L14.5 5M7.5 19h9M8 11.5 10 8M16 11.5 14 8'
});
const ICON_NOFILL=new Set('up down left right ok x undo redo straight curve complex continuous swap replace rotate path elev pipe pylon line grid snap parallel road_s road_m road_l highway rail xing t_xing pause play ff fff srch exit move funding taxes contour flatten dig raise ore'.split(' '));
icon=function(name,cls){const d=ICONS[name]||ICONS.info;const c=ICON_COL[name];
 return `<svg class="ic ${ICON_NOFILL.has(name)?'nf':''} ${cls||''}" viewBox="0 0 24 24"${c?` style="--ic:${c}"`:''}><path d="${d}"/></svg>`};

// ---- more informative category / info icons ----
Object.assign(ICONS,{
 stats:'M3 3v18h18M7 16l4-5 3 3 6-7M16 7h4v4M7 16h.01M11 11h.01M14 14h.01',
 serv:'M12 3l9 5H3zM3 21h18M4.5 18h15M6 10v8M10 10v8M14 10v8M18 10v8M3 8h18',
 power:'M13 2 4 14h7l-1 8 9-12h-7z',
 util:'M10 2 4 12h4.5L7.5 20 14 9.5H9.5zM18 10s-3.5 4-3.5 6.5a3.5 3.5 0 0 0 7 0C21.5 14 18 10 18 10z',
 zone:'M3 3h8v8H3zM13 3h8v8h-8zM3 13h8v8H3zM13 13h8v8h-8zM5 9V6.8L7 5l2 1.8V9z',
 admin:'M3 21h18M5 21V11h14v10M8 11V8.5a4 4 0 0 1 8 0V11M12 4.5V2M9.5 21v-5h5v5',
 city:'M2 21h20M4 21V11l4-3v13M8 21V5h6v16M14 21v-9h3v9M17 21V8h3v13M10 8h2M10 11h2M10 14h2',
 demand:'M4 20V12h4v8M10 20V7h4v13M16 20v-5h4v5M3 20h18M6 9V7m-1 1 1-1 1 1M12 4V2m-1 1 1-1 1 1',
 ppl:'M9 4a3.2 3.2 0 1 0 0 6.4A3.2 3.2 0 0 0 9 4zM2.5 20c0-3.5 3-5.5 6.5-5.5s6.5 2 6.5 5.5M16.5 5a2.8 2.8 0 1 1 0 5.6M18 14.6c2.3.5 3.8 2.3 3.8 5'
});
ICON_COL.util='#facc15';ICON_COL.city='#a78bfa';ICON_COL.serv='#f43f5e';ICON_COL.stats='#2dd4bf';
ICON_NOFILL.add('stats');ICON_NOFILL.add('demand');

// ---- multi-colour icons: an entry can be [[path, colour], ...] ----
ICONS.util=[['M10 2 4 12h4.5L7.5 20 14 9.5H9.5z','#facc15'],['M18 10s-3.5 4-3.5 6.5a3.5 3.5 0 0 0 7 0C21.5 14 18 10 18 10z','#38bdf8']];
// public services: an open hand holding a group of people
ICONS.serv=[['M12 2.5a2 2 0 1 0 0 4 2 2 0 0 0 0-4zM6.5 4a1.8 1.8 0 1 0 0 3.6 1.8 1.8 0 0 0 0-3.6zM17.5 4a1.8 1.8 0 1 0 0 3.6 1.8 1.8 0 0 0 0-3.6zM8.5 12c0-2 1.6-3.5 3.5-3.5s3.5 1.5 3.5 3.5zM3.5 12c0-1.8 1.3-3 3-3 .7 0 1.3.2 1.8.5M20.5 12c0-1.8-1.3-3-3-3-.7 0-1.3.2-1.8.5','#f43f5e'],
 ['M2 15h3v6H2zM5 16h3.5l2.5 1.5h3a1.3 1.3 0 0 1 0 2.6h-3.5M14 19.5l4.8-2.8a1.4 1.4 0 0 1 1.6 2.3L15 22H5','#fbbf77']];
const _iconStr=icon;
icon=function(name,cls){const v=ICONS[name];if(!Array.isArray(v))return _iconStr(name,cls);
 return `<svg class="ic multi ${cls||''}" viewBox="0 0 24 24">${v.map(([d,c])=>`<path d="${d}" style="--ic:${c}"/>`).join('')}</svg>`};
const iconPath=n=>{const v=ICONS[n];return Array.isArray(v)?v.map(x=>x[0]).join(''):v};
