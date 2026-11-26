import QtQuick
// RCAS sharpness slider acceptance: visible only for FSR 3.1 / FSR4, default 20,
// live value reaches the bridge, hidden for other algorithms. List + node modes.
Item {
    id: test
    property string media: ""
    property string shot: ""
    function find(name) {
        let hit=null
        const walk=item=>{if(hit)return;if(item.objectName===name){hit=item;return}for(const c of item.children)walk(c)}
        walk(test.parent.parent);return hit
    }
    function shown(item){let it=item;while(it){if(it.visible===false)return false;it=it.parent}return true}
    Timer {
        interval:500;running:test.media.length>0;repeat:true
        property int step:0
        property int ticks:0
        property int total:0
        function next(n){step=n;ticks=0}
        function check(ok,label){if(!ok)throw new Error(label);veyra.logUi("sr-sharp-test","PASS "+label)}
        function settled(){return ticks>=7&&veyra.running&&!veyra.applying}
        onTriggered:{try{
            ++ticks;if(++total>200)throw new Error("bounded sharpness test step="+step)
            if(step===0){
                if(ticks<4)return
                veyra.nodeMode=0;veyra.navigate("pro")
                veyra.muted=true;veyra.nrEnabled=false;veyra.fgEnabled=false
                veyra.srEnabled=true;veyra.videoSrQuality=5;veyra.dualSrEnabled=false;veyra.srOutput=0;veyra.srTargetIndex=2
                veyra.openPath(test.media);next(1);return
            }
            if(step===1){
                if(!settled())return
                const row=find("list-sr-sharpness")
                check(row!==null,"list sharpness slider exists")
                check(shown(row),"visible for FSR 3.1")
                check(veyra.srSharpness===20,"default 20 percent")
                check(row.value===20,"slider shows 20")
                row.moved(35);next(2);return
            }
            if(step===2){
                if(ticks<4)return
                check(veyra.srSharpness===35,"bridge value 35 after drag")
                check(find("list-sr-sharpness").value===35,"slider binding follows bridge")
                veyra.videoSrQuality=6;next(3);return
            }
            if(step===3){
                if(!settled())return
                const row=find("list-sr-sharpness")
                check(row===null||!shown(row),"hidden for XeSS")
                veyra.videoSrQuality=5;next(4);return
            }
            if(step===4){
                if(!settled())return
                check(shown(find("list-sr-sharpness")),"visible again back on FSR")
                check(veyra.srSharpness===35,"value kept across algorithm switch")
                veyra.srSharpness=0;next(5);return
            }
            if(step===5){
                if(ticks<4)return
                check(veyra.srSharpness===0,"zero accepted (sharpening off)")
                veyra.srSharpness=20
                veyra.nodeMode=1;check(veyra.addEffect("sr")>=0,"add SR node to the node graph");veyra.navigate("node")
                veyra.videoSrQuality=5;next(6);return
            }
            if(step===6){
                if(ticks<6)return
                const row=find("node-sr-sharpness")
                check(row!==null&&shown(row),"node mode sharpness slider visible for FSR")
                check(row.value===20,"node slider shows 20")
                row.moved(50);next(7);return
            }
            if(step===7){
                if(ticks<4)return
                check(veyra.srSharpness===50,"node drag reaches bridge")
                veyra.logUi("sr-sharp-test","SR_SHARP_UI_PASS");console.log("SR_SHARP_UI_PASS");Qt.quit()
                return
            }
        }catch(e){veyra.logUi("sr-sharp-test","SR_SHARP_UI_FAIL "+e.message);console.log("SR_SHARP_UI_FAIL "+e.message);Qt.quit()}}
    }
}