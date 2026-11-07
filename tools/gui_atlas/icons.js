// Basic placeholder icon set: 24x24, stroke-only, currentColor.
// Paths only (no fills), so they theme with any GUI style.
const ICONS={
// ---- top-level categories ----
road:'M8 3 5 21M16 3l3 18M12 5v2m0 4v2m0 4v2',
zone:'M3 3h8v8H3zM13 3h8v8h-8zM3 13h8v8H3zM13 13h8v8h-8z',
terr:'M2 20 9 8l4 6 3-4 6 10z',
power:'M13 2 4 14h7l-1 8 9-12h-7z',
water:'M12 3s-7 8-7 12a7 7 0 0 0 14 0c0-4-7-12-7-12z',
trash:'M4 6h16M9 6V3h6v3M6 6l1 15h10l1-15M10 10v7m4-7v7',
serv:'M10 3h4v7h7v4h-7v7h-4v-7H3v-4h7z',
bus:'M5 4h14v12H5zM5 11h14M8 19v-3m8 3v-3M8 14h.01M16 14h.01',
factory:'M3 21V10l5 3V10l5 3V10l5 3V4h3v17zM7 17h2m3 0h2m3 0h2',
coin:'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM12 6.5v11M15 9.5c0-1-1.3-1.8-3-1.8s-3 .8-3 1.8 1.3 1.7 3 2.2 3 1.2 3 2.3-1.3 1.8-3 1.8-3-.8-3-1.8',
info:'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM12 11v6M12 7.5v.5',
layers:'M12 3l10 5-10 5L2 8zM2 12l10 5 10-5M2 16l10 5 10-5',
// ---- roads ----
road_s:'M9 3v18M15 3v18M12 6v2m0 4v2m0 4v2',
road_m:'M6 3v18M18 3v18M10 5v2m0 4v2m0 4v2M14 5v2m0 4v2m0 4v2',
road_l:'M4 3v18M20 3v18M11.2 3v18M12.8 3v18M7.5 5v2m0 4v2m0 4v2M16.5 5v2m0 4v2m0 4v2',
highway:'M3 21 8 3M21 21 16 3M11 21l.5-4M12.5 7l.3-4M11.7 14l.2-3',
path:'M8 21c0-5 8-6 8-11s-3-7-4-7M7 6h.01M9 9h.01M14 15h.01M16 18h.01',
bike:'M6 13a4 4 0 1 0 0 8 4 4 0 0 0 0-8zM18 13a4 4 0 1 0 0 8 4 4 0 0 0 0-8zM6 17l4-8h5l3 8M10 9 8 6H6m9 3 1-3h2',
quay:'M3 3h10v12H3zM8 5v2m0 3v2M15 15c1.5 0 1.5-1.5 3-1.5s1.5 1.5 3 1.5M15 19c1.5 0 1.5-1.5 3-1.5s1.5 1.5 3 1.5M3 19h9',
rail:'M8 3 6 21M16 3l2 18M6.5 6h11M6 10h12M5.5 14h13M5 18h14',
xing:'M9 3v6H3m6 12v-6H3m12-12v6h6m-6 12v-6h6',
roundabout:'M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8zM12 2v6m0 8v6M2 12h6m8 0h6',
t_xing:'M3 9h18M3 15h6v6m6-6h6m-6 0v6',
// tool modes
straight:'M5 19 19 5M5 19h.01M19 5h.01M3 21l4-4M17 3l4 4',
curve:'M4 20C4 10 10 4 20 4M4 20h.01M20 4h.01',
complex:'M3 20c6 0 4-8 9-8s3-8 9-8',
continuous:'M3 18l5-8 5 6 4-9 4 5',
grid:'M3 8h18M3 16h18M8 3v18M16 3v18',
replace:'M4 12a8 8 0 0 1 14-5l2 2M20 12a8 8 0 0 1-14 5l-2-2M20 4v5h-5M4 20v-5h5',
swap:'M4 8h15l-4-4M20 16H5l4 4',
bulldoze:'M3 17h11v-5H8l-2-4H3zM14 14l7-3v8l-7-2M5 20a1.5 1.5 0 1 0 0-3M12 20a1.5 1.5 0 1 0 0-3',
elev:'M8 4v16M5 7l3-3 3 3M5 17l3 3 3-3M14 12h7M14 8h7M14 16h7',
snap:'M6 3v8a6 6 0 0 0 12 0V3h-4v8a2 2 0 0 1-4 0V3zM6 7h4m4 0h4',
parallel:'M7 3v18M17 3v18M10 12h4M12 10l2 2-2 2',
under:'M2 10h20M5 10v4c0 3 3 5 7 5s7-2 7-5v-4M2 6h20',
rotate:'M20 12a8 8 0 1 1-3-6.2M20 4v5h-5',
// ---- zones ----
house:'M3 11 12 4l9 7M5 10v10h14V10M10 20v-6h4v6',
row:'M2 10l4-4 4 4 4-4 4 4 4-4M3 9v11h18V9M7 20v-4m5 4v-4m5 4v-4',
apartment:'M6 21V5h12v16M9 8h2m2 0h2m-6 4h2m2 0h2m-6 4h2m2 0h2M4 21h16',
shop:'M4 9V20h16V9M3 9l2-5h14l2 5c0 1.5-1.3 2-2.5 2S16 10.5 16 9c0 1.5-1.8 2-4 2S8 10.5 8 9c0 1.5-1.3 2-2.5 2S3 10.5 3 9zM10 20v-5h4v5',
office:'M5 21V3h9v18M14 9h5v12M8 6h3M8 10h3M8 14h3M3 21h18',
industry:'M3 21V11l6 4v-4l6 4V5h3v16zM18 5V3',
dezone:'M15 4l5 5-10 10H5v-5zM12 7l5 5M4 21h16',
brush:'M18 3l3 3-9 9-3-3zM9 12c-3 0-4 2-4 4s-1 3-3 3c2 2 8 2 9-3',
bucket:'M5 9l7-6 7 7-7 7zM5 9c-2 3-2 7 0 7M19 15c0 2 1 3 2 3s2-1 2-3-2-4-2-4-2 2-2 4z',
box:'M4 4h16v16H4zM4 4l3 3M20 20l-3-3',
// ---- terrain ----
raise:'M3 20l6-9 4 5 3-3 5 7zM16 3v6M13 6l3-3 3 3',
dig:'M3 9h6l3 6 3-6h6M12 3v6M9 6l3 3 3-3M3 20h18',
flatten:'M3 15h18M3 19h18M6 11l3-4 3 3 3-2 3 3',
contour:'M4 14c0-6 4-9 8-9s8 3 8 9M7 15c0-4 2-6 5-6s5 2 5 6M10 16c0-2 1-3 2-3s2 1 2 3M2 19h20',
tree:'M12 3l6 9h-3l4 6H5l4-6H6zM12 18v3',
// ---- electricity ----
wind:'M12 11v10M9 21h6M12 11 12 3M12 11l7 4M12 11l-7 4M12 9.5a1.5 1.5 0 1 0 0 3',
solar:'M3 17 6 7h12l3 10zM4.5 12h15M10 7l-1 10M14 7l1 10M12 17v4M8 21h8',
coal:'M4 21V11l5 3v-3l5 3V7h4v14zM15 7c0-2 2-2 2-4M19 7c0-2 2-2 2-4',
gas:'M12 3c3 4 5 6 5 10a5 5 0 0 1-10 0c0-2 1-4 2-5 0 2 1 3 2 3 0-3 1-5 1-8zM5 21h14',
geo:'M3 21h18M6 21v-6h12v6M9 15c0-3-2-4-2-7M13 15c0-3 2-4 2-7M11 10c0-2-1-3-1-5',
nuclear:'M12 10.5a1.5 1.5 0 1 0 0 3 1.5 1.5 0 0 0 0-3zM12 3c5 0 4 9 0 9s-5-9 0-9zM4.2 16.5c-2.5-4.3 5.3-8.8 7.8-4.5s-5.3 8.8-7.8 4.5zM19.8 16.5c-2.5 4.3-10.3-.2-7.8-4.5s10.3.2 7.8 4.5z',
generator:'M3 8h18v10H3zM7 18v2m10-2v2M13 9.5 10 13h4l-3 3.5',
battery:'M3 7h16v10H3zM19 10h2v4h-2M7 10v4m4-4v4',
pylon:'M12 2 7 22M12 2l5 20M8 15h8M9 10h6M4 7h16M7 22l10-7M17 22 7 15',
// ---- water ----
tower:'M7 3h10v7H7zM8 10l-2 11M16 10l2 11M7 15h10M12 6v1',
pump:'M4 21V9h8v12M12 13h6V7M15 7h6M8 9V5h4M4 21h16',
groundwater:'M8 3v12M4 9h8M8 15c-2 2-3 3-3 4a3 3 0 0 0 6 0c0-1-1-2-3-4zM14 20h7M16 17h5',
sewage:'M3 6h10v4H3zM13 8h4c2 0 3 1 3 3v3M18 17s-2 2-2 3a2 2 0 0 0 4 0c0-1-2-3-2-3zM3 14h7',
treatment:'M3 13a9 4 0 0 0 18 0M3 13a9 4 0 0 1 18 0M3 13v4a9 4 0 0 0 18 0v-4M12 3s-2 2.5-2 3.5a2 2 0 0 0 4 0C14 5.5 12 3 12 3z',
pipe:'M3 9h9a4 4 0 0 1 4 4v8M3 15h9M20 21v-8a8 8 0 0 0-8-8H3',
// ---- garbage ----
landfill:'M2 20l5-8 4 3 4-6 7 11zM2 20h20M14 6h4l-1-3h-2z',
incinerator:'M4 21V11h12v10M16 21V5h3v16M7 21v-4h6v4M8 11c0-2 2-2 2-4M12 11c0-2 2-2 2-4',
recycle:'M7 19H4l3-5M17 19h3l-3-5M12 4l-2 3M12 4l2 3M8.5 12 12 6l3.5 6M7 19h10M10 10l-2 2M14 10l2 2',
hazard:'M12 3 2 20h20zM12 9v5M12 17v.5',
// ---- services ----
health:'M10 3h4v7h7v4h-7v7h-4v-7H3v-4h7z',
death:'M6 21V9a6 6 0 0 1 12 0v12M4 21h16M12 9v6M10 11h4',
fire:'M12 3c3 4 6 6 6 11a6 6 0 0 1-12 0c0-3 2-5 3-6 0 2 1 4 3 4-1-3 0-6 0-9z',
police:'M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6zM9 12l2 2 4-4',
school:'M2 9l10-5 10 5-10 5zM6 11v5c3 2 9 2 12 0v-5M21 9v6',
comms:'M12 10v11M9 21h6M12 10l-3 11M12 10l3 11M8 6a6 6 0 0 1 8 0M5 3a10 10 0 0 1 14 0M12 8.5a1.5 1.5 0 1 0 0 3',
park:'M8 3l4 7H9l3 5H4l3-5H4zM8 15v6M16 8a3 3 0 1 0 0 6 3 3 0 0 0 0-6zM16 14v7M3 21h18',
admin:'M3 21h18M5 21V10h14v11M3 10l9-6 9 6M9 21v-6h6v6',
// ---- transport ----
train:'M6 3h12v12H6zM6 9h12M9 15l-3 5M15 15l3 5M8 18h8M9 12h.01M15 12h.01',
ship:'M3 15l2 5h14l2-5zM6 15V9h12v6M10 9V5h4v4M3 21c2 0 2-1 4.5-1S10 21 12 21s2-1 4.5-1 2.5 1 4.5 1',
plane:'M21 16v-2l-8-5V4a1.5 1.5 0 0 0-3 0v5l-8 5v2l8-2.5V19l-2 1.5V22l3.5-1 3.5 1v-1.5L13 19v-5.5z',
tram:'M6 6h12v11H6zM6 11h12M9 3h6M12 3v3M9 17l-2 4M15 17l2 4',
taxi:'M4 16v-4l2-5h12l2 5v4zM2 16h20M7 16v3m10-3v3M10 4h4v3h-4zM7 13h.01M17 13h.01',
subway:'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM7 16V8l5 6 5-6v8',
line:'M5 19a2 2 0 1 0 0-4 2 2 0 0 0 0 4zM19 9a2 2 0 1 0 0-4 2 2 0 0 0 0 4zM12 14a2 2 0 1 0 0-4 2 2 0 0 0 0 4zM6.5 15.5l4-3M13.7 10.8l3.6-2.6',
cargo:'M3 8l9-5 9 5v8l-9 5-9-5zM3 8l9 5 9-5M12 13v8',
depot:'M3 21V9l9-6 9 6v12M7 21v-7h10v7M7 17h10',
outside:'M3 3h10v18H3zM16 8l4 4-4 4M9 12h11',
// ---- industry ----
grain:'M12 21V8M12 8c-3 0-4-3-4-5 3 0 4 2 4 5zM12 8c3 0 4-3 4-5-3 0-4 2-4 5zM12 13c-3 0-4-3-4-5 3 0 4 2 4 5zM12 13c3 0 4-3 4-5-3 0-4 2-4 5zM12 18c-3 0-4-3-4-5 3 0 4 2 4 5zM12 18c3 0 4-3 4-5-3 0-4 2-4 5z',
livestock:'M5 7 3 4M19 7l2-3M6 8h12v6a6 6 0 0 1-12 0zM9 11h.01M15 11h.01M10 16h4',
forestry:'M7 3l4 7H8l3 5H3l3-5H3zM7 15v3M13 20h8M14 17h6a1.5 1.5 0 0 1 0 3M14 17a1.5 1.5 0 0 0 0 3',
oil:'M8 21 12 3l4 18M9.5 14h5M10.5 9h3M5 21h14M12 3l6 4',
ore:'M14 4c3 1 5 3 6 6M14 4 4 20l2 1 10-15M14 4l2 2M3 21h4',
area:'M4 6l6-3 10 4-2 11-9 3-5-7zM4 6h.01M10 3h.01M20 7h.01M18 18h.01M9 21h.01M4 14h.01',
// ---- city management ----
taxes:'M19 5 5 19M7 5a2 2 0 1 0 0 4 2 2 0 0 0 0-4zM17 15a2 2 0 1 0 0 4 2 2 0 0 0 0-4z',
funding:'M4 21V11m5 10V7m5 14v-8m5 8V4',
loan:'M3 10h18M5 10v8m4-8v8m6-8v8m4-8v8M3 21h18M3 10l9-6 9 6',
policy:'M6 3h9l4 4v14H6zM15 3v4h4M9 11h7M9 15h7M9 19h4',
district:'M4 5l7-2 9 4-1 12-9 2-6-6z',
tiles:'M3 3h8v8H3zM13 13h8v8h-8zM3 13h8v8H3zM17 4v6M14 7h6',
milestone:'M5 21V3M5 4h12l-2 4 2 4H5',
// ---- info ----
stats:'M4 20V4M4 20h16M8 16v-5M12 16V8M16 16v-3M20 16V6',
ppl:'M12 4a4 4 0 1 0 0 8 4 4 0 0 0 0-8zM4 21c0-4 4-6 8-6s8 2 8 6',
citizen:'M3 5h18v14H3zM9 9a2 2 0 1 0 0 4 2 2 0 0 0 0-4zM5.5 17c.5-2 2-3 3.5-3s3 1 3.5 3M14 9h4M14 13h4',
bell:'M6 16V11a6 6 0 0 1 12 0v5l2 2H4zM10 20a2 2 0 0 0 4 0',
radio:'M3 9h18v12H3zM7 9l10-6M8 13a2 2 0 1 0 0 4 2 2 0 0 0 0-4zM14 13h4M14 17h4',
advisor:'M4 4h16v12H9l-5 4zM8 8h8M8 12h5',
demand:'M5 20V10m0 10h4V6H5m6 14V13h4v7m2 0V16h4v4',
heart:'M12 20s-8-5-8-11a4.5 4.5 0 0 1 8-2.5A4.5 4.5 0 0 1 20 9c0 6-8 11-8 11z',
// ---- views ----
traffic:'M8 2h8v20H8zM12 5a1.5 1.5 0 1 0 0 3M12 10.5a1.5 1.5 0 1 0 0 3M12 16a1.5 1.5 0 1 0 0 3',
pollution:'M4 15a4 4 0 0 1 2-7.5A5 5 0 0 1 15.5 6 4 4 0 0 1 20 13.5 3.5 3.5 0 0 1 17.5 19H7a3.5 3.5 0 0 1-3-4zM8 22h.01M12 22h.01M16 22h.01',
landvalue:'M3 11 12 4l9 7M5 10v10h14V10M12 11v6M14 12.5c0-.8-.9-1.3-2-1.3s-2 .5-2 1.3.9 1.2 2 1.4 2 .7 2 1.5-.9 1.3-2 1.3-2-.5-2-1.3',
noise:'M4 9h4l5-4v14l-5-4H4zM16 9a4 4 0 0 1 0 6M19 6a8 8 0 0 1 0 12',
resource:'M12 2l3 6 6 1-4 5 1 7-6-3-6 3 1-7-4-5 6-1z',
// ---- system ----
pause:'M8 5v14M16 5v14',
play:'M7 4l13 8-13 8z',
ff:'M3 5l9 7-9 7zM12 5l9 7-9 7z',
fff:'M2 6l6 6-6 6zM9 6l6 6-6 6zM16 6l6 6-6 6z',
save:'M5 3h11l3 3v15H5zM8 3v6h8V3M8 21v-7h8v7',
folder:'M3 6h6l2 2h10v11H3z',
newmap:'M3 6l6-3 6 3 6-3v15l-6 3-6-3-6 3zM9 3v15M15 6v15',
gear:'M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6zM12 2v3m0 14v3M2 12h3m14 0h3M4.9 4.9l2.1 2.1m10 10 2.1 2.1M4.9 19.1 7 17M17 7l2.1-2.1',
cam:'M4 7h4l2-3h4l2 3h4v13H4zM12 10a3.5 3.5 0 1 0 0 7 3.5 3.5 0 0 0 0-7z',
walk:'M13 4a2 2 0 1 0 0 .01M10 21l2-6 3 3v3M9 12l2-4 4 2 2 3M11 8l-2 5',
fly:'M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6zM2 12s4-7 10-7 10 7 10 7-4 7-10 7S2 12 2 12z',
person:'M12 3a3 3 0 1 0 0 6 3 3 0 0 0 0-6zM6 21v-5a6 6 0 0 1 12 0v5',
hide:'M3 3l18 18M10.6 10.6a2 2 0 0 0 2.8 2.8M9.4 5.2A10 10 0 0 1 12 5c6 0 10 7 10 7a17 17 0 0 1-3.2 3.8M6.6 6.6C3.9 8.3 2 12 2 12s4 7 10 7c1.7 0 3.3-.5 4.6-1.3',
focus:'M3 8V3h5M16 3h5v5M21 16v5h-5M8 21H3v-5M12 9a3 3 0 1 0 0 6 3 3 0 0 0 0-6z',
volume:'M4 9h4l5-4v14l-5-4H4zM16 9a4 4 0 0 1 0 6',
display:'M3 4h18v12H3zM8 20h8M12 16v4',
exit:'M14 4h6v16h-6M10 8l-4 4 4 4M6 12h10',
undo:'M9 14 4 9l5-5M4 9h11a5 5 0 0 1 0 10h-3',
redo:'M15 14l5-5-5-5M20 9H9a5 5 0 0 0 0 10h3',
ok:'M5 12l5 5 9-10', x:'M6 6l12 12M18 6 6 18',
srch:'M11 4a7 7 0 1 0 0 14 7 7 0 0 0 0-14zM20 20l-4-4',
clock:'M12 3a9 9 0 1 0 0 18 9 9 0 0 0 0-18zM12 7v5l3 2',
alert:'M12 3 2 20h20zM12 9v5m0 3v.5',
sun:'M12 8a4 4 0 1 0 0 8 4 4 0 0 0 0-8zM12 2v2m0 16v2M2 12h2m16 0h2M5 5l1.5 1.5m11 11L19 19M5 19l1.5-1.5m11-11L19 5',
rain:'M6 14a4 4 0 0 1 1-7.9A5 5 0 0 1 16.5 5 4 4 0 0 1 18 13H6zM8 17l-1 3M12 17l-1 3M16 17l-1 3',
star:'M12 3l2.8 5.8 6.2.9-4.5 4.4 1 6.3L12 17.5 6.5 20.4l1-6.3L3 9.7l6.2-.9z',
up:'M6 15l6-6 6 6', down:'M6 9l6 6 6-6', left:'M15 6l-6 6 6 6', right:'M9 6l6 6-6 6',
move:'M12 3v18M3 12h18M12 3l-3 3m3-3 3 3M12 21l-3-3m3 3 3-3M3 12l3-3m-3 3 3 3M21 12l-3-3m3 3-3 3',
lock:'M5 11h14v10H5zM8 11V7a4 4 0 0 1 8 0v4'
};
function icon(name,cls){const d=ICONS[name]||ICONS.info;return `<svg class="ic ${cls||''}" viewBox="0 0 24 24"><path d="${d}"/></svg>`}

// ---- colours: semantic per icon (fallback: neutral) ----
const ICON_COL={};
(function(){const set=(c,names)=>names.split(' ').forEach(n=>ICON_COL[n]=c);
set('#9aa3b5','road road_s road_m road_l t_xing xing roundabout straight curve complex continuous grid replace swap elev snap parallel rotate under');
set('#f59e0b','highway');set('#c08457','path quay');set('#a8a29e','rail');set('#fb923c','bulldoze dezone');set('#34d399','bike');
set('#4ade80','zone house row apartment brush bucket box');set('#60a5fa','shop');set('#a78bfa','office');set('#facc15','industry');
set('#d4a373','terr raise dig flatten contour');set('#22c55e','tree park forestry');
set('#facc15','power wind solar coal gas geo nuclear generator battery pylon');set('#38bdf8','water tower pump groundwater pipe treatment');set('#a16207','sewage');
set('#a3e635','trash landfill recycle');set('#f97316','incinerator hazard');
set('#f43f5e','serv health');set('#94a3b8','death');set('#ef4444','fire');set('#6366f1','police');set('#c084fc','school');set('#2dd4bf','comms');set('#e2c08d','admin');
set('#3b82f6','bus tram taxi line');set('#f87171','train subway depot');set('#0ea5e9','ship');set('#e5e7eb','plane');set('#d97706','cargo');set('#8b5cf6','outside');
set('#fb923c','factory area');set('#eab308','grain');set('#f5d0a9','livestock');set('#1f2937','oil');set('#9ca3af','ore');
set('#a78bfa','coin taxes funding loan policy district tiles');set('#fbbf24','milestone star');
set('#2dd4bf','info stats ppl citizen demand');set('#f87171','bell alert');set('#f472b6','radio heart');set('#93c5fd','advisor');
set('#818cf8','layers');set('#ef4444','traffic');set('#9ca3af','pollution');set('#22c55e','landvalue');set('#c084fc','noise');set('#fb923c','resource');
set('#fde047','sun');set('#93c5fd','rain');set('#4ade80','ok play');set('#f87171','x exit');
ICON_COL.oil='#64748b';})();
function icon(name,cls){const d=ICONS[name]||ICONS.info;const c=ICON_COL[name];
 return `<svg class="ic ${cls||''}" viewBox="0 0 24 24"${c?` style="--ic:${c}"`:''}><path d="${d}"/></svg>`}
