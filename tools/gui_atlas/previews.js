// Pseudo-3D isometric thumbnails for build items (stand-ins for real model renders).
// preview(iconName, itemName, cost) -> <svg> string, viewBox 64x64.
(function(){
const K=1.0;                                   // iso scale
const P=(i,j,z)=>[32+(i-j)*0.87*K, 40+(i+j)*0.5*K - z];
const pts=a=>a.map(p=>p.map(v=>v.toFixed(1)).join(',')).join(' ');
function sh(hex,f){const n=parseInt(hex.slice(1),16);let r=n>>16,g=n>>8&255,b=n&255;
 const m=v=>Math.max(0,Math.min(255,Math.round(f>1?v+(255-v)*(f-1):v*f)));return `rgb(${m(r)},${m(g)},${m(b)})`}
function box(i0,j0,w,d,z0,h,col,o=''){const i1=i0+w,j1=j0+d;
 const top=[P(i0,j0,z0+h),P(i1,j0,z0+h),P(i1,j1,z0+h),P(i0,j1,z0+h)];
 const L=[P(i0,j1,z0),P(i1,j1,z0),P(i1,j1,z0+h),P(i0,j1,z0+h)];
 const R=[P(i1,j0,z0),P(i1,j1,z0),P(i1,j1,z0+h),P(i1,j0,z0+h)];
 return `<polygon points="${pts(L)}" fill="${sh(col,.78)}"${o}/><polygon points="${pts(R)}" fill="${sh(col,.6)}"${o}/><polygon points="${pts(top)}" fill="${sh(col,1.12)}"${o}/>`}
function roof(i0,j0,w,d,z,h,col){const i1=i0+w,j1=j0+d,jm=(j0+j1)/2;
 return `<polygon points="${pts([P(i1,j0,z),P(i1,j1,z),P(i1,jm,z+h)])}" fill="${sh(col,.65)}"/><polygon points="${pts([P(i0,j1,z),P(i1,j1,z),P(i1,jm,z+h),P(i0,jm,z+h)])}" fill="${sh(col,.95)}"/>`}
function windows(i0,j0,w,d,z0,h,rows){let s='';const j1=j0+d,i1=i0+w;
 for(let r=0;r<rows;r++){const z=z0+3+r*(h-4)/rows;
  for(let c=0;c<3;c++){const i=i0+2+c*(w-4)/3;s+=`<polygon points="${pts([P(i,j1,z),P(i+(w-4)/3-1.5,j1,z),P(i+(w-4)/3-1.5,j1,z+1.8),P(i,j1,z+1.8)])}" fill="#fff3c4" opacity=".75"/>`}
  for(let c=0;c<2;c++){const j=j0+2+c*(d-4)/2;s+=`<polygon points="${pts([P(i1,j,z),P(i1,j+(d-4)/2-1.5,z),P(i1,j+(d-4)/2-1.5,z+1.8),P(i1,j,z+1.8)])}" fill="#ffe9a8" opacity=".5"/>`}}
 return s}
const ground=(c='#6f9a52')=>box(-20,-20,40,40,-3,3,c);
function road(lanes,kind){
 const asph=kind==='rail'?'#7a6a58':kind==='path'?'#c9b48c':kind==='quay'?'#8a8f99':'#4a4f5c';
 let s=ground(kind==='quay'?'#3f86c9':'#6f9a52')+box(-22,-7,44,14,0,1.2,asph);
 if(kind==='rail'){for(let i=-20;i<=20;i+=4)s+=box(i,-6,1.6,12,1.2,.6,'#5b4636');s+=box(-22,-3.5,44,.8,1.8,.8,'#c7ccd6')+box(-22,2.7,44,.8,1.8,.8,'#c7ccd6');return s}
 if(kind==='path'){for(let i=-18;i<=18;i+=6)s+=`<circle cx="${P(i,0,1.3)[0]}" cy="${P(i,0,1.3)[1]}" r=".8" fill="#7a6a4a"/>`;return s}
 const n=Math.max(1,lanes);for(let k=1;k<n;k++){const j=-7+14*k/n;for(let i=-20;i<20;i+=6){const a=P(i,j,1.3),b=P(i+3.5,j,1.3);s+=`<line x1="${a[0]}" y1="${a[1]}" x2="${b[0]}" y2="${b[1]}" stroke="${kind==='hwy'&&k==n/2?'#f5c542':'#fff'}" stroke-width=".8"/>`}}
 s+=box(-22,-8.5,44,1.5,0,2,'#b9bec8')+box(-22,7,44,1.5,0,2,'#b9bec8');return s}
function cross(round){let s=ground()+box(-22,-6,44,12,0,1.2,'#4a4f5c')+box(-6,-22,12,44,0,1.2,'#4a4f5c');
 if(round){s+=box(-8,-8,16,16,0,1.25,'#4a4f5c');const c=P(0,0,1.4);s+=`<ellipse cx="${c[0]}" cy="${c[1]}" rx="7" ry="3.8" fill="#6fae52"/>`}return s}
function tree(i,j,s=1){const b=P(i,j,0),t=P(i,j,14*s);return `<line x1="${b[0]}" y1="${b[1]}" x2="${b[0]}" y2="${b[1]-4*s}" stroke="#6b4a2b" stroke-width="1.6"/><polygon points="${t[0]},${t[1]} ${b[0]-5*s},${b[1]-3*s} ${b[0]+5*s},${b[1]-3*s}" fill="#2f8a43"/><polygon points="${t[0]},${t[1]} ${b[0]},${b[1]-2*s} ${b[0]+5*s},${b[1]-3*s}" fill="#226b33"/>`}
function chimney(i,j,z,h,smoke=true){const t=P(i,j,z+h);return box(i,j,3,3,z,h,'#8a8f99')+(smoke?`<circle cx="${t[0]+3}" cy="${t[1]-4}" r="3" fill="#d6d8dc" opacity=".7"/><circle cx="${t[0]+6}" cy="${t[1]-9}" r="4" fill="#e4e6ea" opacity=".5"/>`:'')}
function emblem(name,col){const c=(typeof iconPath!=='undefined')?iconPath(name):(typeof ICONS!=='undefined'?ICONS:{})[name];if(!c)return'';return `<g transform="translate(43 3) scale(.75)"><rect x="-1" y="-1" width="26" height="26" rx="8" fill="#0009"/><path d="${c}" fill="none" stroke="${col}" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></g>`}
function costH(cost){const m=/([\d.]+)\s*k/i.exec(cost||'');const v=m?parseFloat(m[1]):10;return Math.max(8,Math.min(30,6+Math.log2(v+1)*3.2))}

window.preview=function(ic,name,cost){
 const col=(typeof ICON_COL!=='undefined'?ICON_COL:{})[ic]||'#9aa3b5';let s='';const nm=(name||'').toLowerCase();
 const lanes=/eight/.test(nm)?8:/seven/.test(nm)?7:/six/.test(nm)?6:/five/.test(nm)?5:/four/.test(nm)?4:/three/.test(nm)?3:/one-lane|alley|gravel/.test(nm)?1:2;
 switch(ic){
 case 'road_s':case 'road_m':case 'road_l':s=road(lanes,'road');break;
 case 'highway':s=road(lanes,'hwy');break;
 case 'path':case 'bike':s=road(1,'path');break;
 case 'quay':s=road(2,'quay');break;
 case 'rail':s=road(2,'rail');break;
 case 'xing':case 't_xing':s=cross(false);break;
 case 'roundabout':s=cross(true);break;
 case 'house':s=ground()+box(-8,-7,16,14,0,9,'#e8dcc8')+roof(-9,-8,18,16,9,7,'#b5523b')+windows(-8,-7,16,14,0,9,1);break;
 case 'row':s=ground();for(let k=0;k<3;k++)s+=box(-14+k*9,-6,8,12,0,10,['#d9c6a5','#c9a98a','#e3d3bb'][k])+roof(-14+k*9,-6,8,12,10,5,'#8f3f2e');break;
 case 'apartment':{const h=/high/.test(nm)?30:18;s=ground()+box(-9,-8,18,16,0,h,'#d8d2c6')+windows(-9,-8,18,16,0,h,Math.round(h/5))+box(-9,-8,18,16,h,1.5,'#7b8494');break}
 case 'shop':s=ground('#8a8f99')+box(-11,-8,22,16,0,10,'#e9e4da')+box(-11,8,22,2.5,6,1.2,'#3b82f6')+windows(-11,-8,22,16,0,10,1);break;
 case 'office':s=ground('#8a8f99')+box(-8,-8,16,16,0,36,'#9fb8d3')+windows(-8,-8,16,16,0,36,8);break;
 case 'industry':case 'factory':s=ground('#8b8a7a')+box(-14,-9,22,18,0,10,'#b9b29d')+chimney(10,-6,0,22)+box(-14,-9,22,18,10,1,'#7a7465');break;
 case 'raise':case 'dig':case 'flatten':case 'contour':case 'terr':{const z=ic==='dig'?-6:ic==='flatten'?0:9;s=box(-20,-20,40,40,-3,3,'#8fae6a')+`<polygon points="${pts([P(-14,-14,0),P(14,-14,0),P(0,0,z+6),])}" fill="${ic==='dig'?'#6b4f33':'#a3bf7a'}"/><polygon points="${pts([P(14,-14,0),P(14,14,0),P(0,0,z+6)])}" fill="${ic==='dig'?'#5a422b':'#86a35f'}"/>`;break}
 case 'tree':case 'park':s=ground()+tree(-8,-6)+tree(6,-8,.8)+tree(0,6,1.1)+(ic==='park'?box(4,4,8,3,0,1.5,'#a87c4f'):'');break;
 case 'forestry':s=ground()+tree(-10,-8)+tree(-2,-12,.9)+tree(8,-4)+box(-4,6,12,3,0,3,'#8a5a32')+box(-4,9.5,12,3,0,3,'#9a6a3e');break;
 case 'wind':{const b=P(0,0,0),t=P(0,0,34);s=ground()+`<line x1="${b[0]}" y1="${b[1]}" x2="${t[0]}" y2="${t[1]}" stroke="#eef0f3" stroke-width="2.4"/>`;for(let a=0;a<3;a++){const r=a*2.094-1.57;s+=`<line x1="${t[0]}" y1="${t[1]}" x2="${t[0]+Math.cos(r)*15}" y2="${t[1]+Math.sin(r)*15}" stroke="#fff" stroke-width="2.2" stroke-linecap="round"/>`}s+=`<circle cx="${t[0]}" cy="${t[1]}" r="2" fill="#cfd4dc"/>`;break}
 case 'solar':s=ground();for(let r=0;r<3;r++)s+=box(-16,-14+r*10,32,6,2,2.5,'#2b4a8a');break;
 case 'pylon':{const b1=P(-4,0,0),b2=P(4,0,0),t=P(0,0,34);s=ground()+`<polyline points="${b1[0]},${b1[1]} ${t[0]},${t[1]} ${b2[0]},${b2[1]}" fill="none" stroke="#9aa3b5" stroke-width="1.6"/><line x1="${t[0]-12}" y1="${t[1]+6}" x2="${t[0]+12}" y2="${t[1]+6}" stroke="#9aa3b5" stroke-width="1.6"/><path d="M0 ${t[1]+8} Q 20 ${t[1]+14} 64 ${t[1]+4}" stroke="#333" stroke-width=".6" fill="none"/>`;break}
 case 'coal':case 'gas':case 'geo':case 'incinerator':s=ground('#8b8a7a')+box(-14,-8,18,16,0,12,ic==='gas'?'#c9ccd2':'#7d7468')+chimney(6,-8,0,26)+chimney(10,-2,0,20,ic!=='gas');break;
 case 'nuclear':{s=ground('#8b8a7a')+box(-14,-6,10,12,0,10,'#d6d9de');const c=P(8,0,0);s+=`<path d="M${c[0]-9} ${c[1]} Q${c[0]-6} ${c[1]-16} ${c[0]-8} ${c[1]-28} L${c[0]+8} ${c[1]-28} Q${c[0]+6} ${c[1]-16} ${c[0]+9} ${c[1]} Z" fill="#e3e5e9"/><path d="M${c[0]} ${c[1]} Q${c[0]+6} ${c[1]-16} ${c[0]+8} ${c[1]-28} L${c[0]+9} ${c[1]-28} Q${c[0]+7} ${c[1]-16} ${c[0]+9} ${c[1]} Z" fill="#b8bcc4"/><circle cx="${c[0]+4}" cy="${c[1]-34}" r="5" fill="#fff" opacity=".7"/>`;break}
 case 'tower':{const b=P(0,0,0),t=P(0,0,22);s=ground()+`<line x1="${b[0]}" y1="${b[1]}" x2="${t[0]}" y2="${t[1]}" stroke="#9aa3b5" stroke-width="3"/><ellipse cx="${t[0]}" cy="${t[1]-4}" rx="10" ry="7" fill="#38bdf8"/><ellipse cx="${t[0]-3}" cy="${t[1]-6}" rx="4" ry="2.5" fill="#bae6fd"/>`;break}
 case 'pipe':s=ground('#7a6a58')+box(-22,-2,44,4,1,4,'#38bdf8');break;
 case 'treatment':case 'sewage':case 'pump':case 'groundwater':{s=ground();const c=P(-4,0,1);s+=box(4,-12,12,10,0,10,'#cfd4dc')+`<ellipse cx="${c[0]}" cy="${c[1]}" rx="13" ry="7" fill="${ic==='sewage'?'#8a6a3a':'#38bdf8'}" stroke="#d6d9de" stroke-width="2"/>`;break}
 case 'landfill':s=ground('#7d6a4f')+`<polygon points="${pts([P(-16,-16,0),P(16,-16,0),P(0,0,12)])}" fill="#a3a17a"/><polygon points="${pts([P(16,-16,0),P(16,16,0),P(0,0,12)])}" fill="#7f7d5c"/>`;break;
 case 'grain':s=box(-20,-20,40,40,-3,3,'#d9b44a');for(let r=-16;r<16;r+=5)s+=box(-18,r,36,2.5,0,1.5,'#c99a2e');s+=box(8,8,8,8,0,12,'#b54a3b');break;
 case 'livestock':s=ground()+box(-12,-8,16,12,0,9,'#b5523b')+roof(-13,-9,18,14,9,5,'#6b2f24')+box(8,4,4,2,0,3,'#f5f5f5')+box(4,10,4,2,0,3,'#3a2a1a');break;
 case 'oil':{const b=P(0,0,0);s=ground('#8b8a7a')+`<polygon points="${b[0]-8},${b[1]} ${b[0]},${b[1]-30} ${b[0]+8},${b[1]}" fill="none" stroke="#64748b" stroke-width="2"/>`+box(6,4,10,4,0,6,'#334155');break}
 case 'ore':s=box(-20,-20,40,40,-3,3,'#8a7d6b')+`<polygon points="${pts([P(-14,-14,0),P(14,-14,0),P(0,0,-8)])}" fill="#5f5546"/>`+box(6,6,8,5,0,5,'#facc15');break;
 default:{const h=costH(cost);s=ground()+box(-10,-9,20,18,0,h,'#d9d5cc')+windows(-10,-9,20,18,0,h,Math.max(1,Math.round(h/7)))+box(-10,-9,20,18,h,2,col);
   if(/hospital|medical|clinic/.test(nm))s+=box(-3,-2,6,4,h+2,1,'#fff');}
 }
 return `<svg class="pv" viewBox="0 0 64 64"><defs><radialGradient id="pvg" cx=".5" cy=".75" r=".6"><stop offset="0" stop-color="#fff" stop-opacity=".18"/><stop offset="1" stop-color="#fff" stop-opacity="0"/></radialGradient></defs><rect width="64" height="64" fill="url(#pvg)"/>${s}${emblem(ic,col)}</svg>`;
};
})();
