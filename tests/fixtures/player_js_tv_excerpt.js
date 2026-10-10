// Trimmed from the real TV player JS (tv-player-ias.vflset, 2026-10-10):
// the string table its obfuscated helpers index, the operation table of
// splice/reverse/swap, the transform that drives it, and one real call site
// (the signature setter). Self-contained; nothing else is needed.
var e='call;slice;length;C;fallback_count;cmo=pf;url;1969-12-31T16:45:46.000-07:15;s;\\.a1\\.googlevideo\\.com$;indexOf;youtube.player.web_20261006_09_RC00;V;path;scheme;,;get;L;prototype;set;sp;local;/file/index.m3u8;Ul;clone;split;j;undefined;reverse;Untrusted URL;signatureCipher;;n;file;push;fromCharCode;replace;/;%3D;mn;/videoplayback;assign;Te;join;splice;1970-01-01T02:30:41.000+02:30;=;toString;:;cmo=td;/api/manifest;startsWith;match;1;forEach;1969-12-31T19:00:56.000-05:00;rr;index.m3u8;/initplayback;Wt;1969-12-31T17:15:05.000-06:45;a1.googlevideo.com;://;r;---;J5;cmo=;fvip;rr?[1-9].*\\.c\\.youtube\\.com$;\\.googlevideo\\.com$;dl;http://local;pop;playerfallback;&;?;BE;joKnMUOudbQ_KEBg97-_w8_;cmo;Ty;N;unshift;redirector.googlevideo.com;//;debugger;www.youtube.com;https://local;\\,""'.split(";");
var E9={dl:function(F,w){F[e[44]](0,w)},
J5:function(F){F[e[28]]()},
Ul:function(F,w){var a=F[0];F[0]=F[w%F[e[2]]];F[w%F[e[2]]]=a}};
var We=function(F,w,a,m){var g=w^F;
if((F&78)==F){var v=a[e[g^3440]](e[g^3446]);E9[e[g^3454]](v,g^3411);E9[e[g^3375]](v,2);E9[e[g^3368]](v,3);E9[e[g^3454]](v,g^3415);E9[e[g^3368]](v,g^3443);var x=v[e[g^3394]](e[g^3446])}F<<1&7||(a=(a%m[e[2]]+m[e[2]])%m[e[2]],m[e[g^332]](-a)[e[g^380]]()[e[g^342]](function(h){m[e[g^305]](h)}));
((F|9)&15)>=10&&(F^54)<24&&(x=encodeURIComponent(a));if(!((F^36)>>4)){if(!(RX(a[e[3]])||a[e[3]][e[g^479]](e[g^505])||a[e[3]][e[g^479]](e[g^442])||a[e[3]][e[g^479]](e[g^427])))throw new dw(e[g^497],a[e[3]]);v=V_(a[e[3]]);a[e[g^482]]=v[e[g^509]];a[e[g^480]]=v[e[g^502]]+(v[e[g^444]]!=null?e[g^476]+v[e[g^444]]:e[g^499]);var r=v[e[3]];r[e[g^479]](e[g^452])?(a[e[g^481]]=e[g^452],r=r[e[1]](g^482)):r[e[g^479]](e[g^470])?(a[e[g^481]]=e[g^470],r=r[e[1]](g^481)):r[e[g^479]](e[g^478])?(r=Fu(g^440,g^4143,r,a),
r=r[0]):(a[e[g^481]]=r,r=e[g^499]);var H=a[e[g^502]];a[e[g^502]]=S8(2,g^8154,r);Object[e[g^453]](a[e[g^502]],U9(g^499,g^5196,v[e[g^480]][e[g^451]]()));Object[e[g^453]](a[e[g^502]],H);a[e[g^502]][e[g^461]]===e[g^469]&&(delete a[e[g^502]][e[g^461]],a[e[g^481]]+=e[g^506]);a[e[3]]=e[g^499];a[e[6]]=e[g^499];a[e[g^509]]&&(v=fa(6,g^6427,a));x=[]}return x};
function BM(F,w,a){w=w===void 0?"":w;a=a===void 0?"":a;F=new Ka(F,!0);F.set("alr","yes");a&&(a=We(6,3439,Gz(37,6180,a)),F[e[19]](w,We(50,6453,a)));return F}
