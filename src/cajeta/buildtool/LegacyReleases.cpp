// The releases olla published before it signed release metadata
// (2026-09-03), which a default client admits on these exact bytes.
//
// Measured 2026-10-20 against https://olla.cajeta.dev: every release whose
// /v2/resolve carried no `signed` envelope, each archive downloaded from
// /v2/blob and hashed. All 49 matched the sha256 olla states; they were
// published 2026-07-19 to 2026-09-03. The set is closed: olla signs every
// publish now, so a new unsigned release means a missing signing key, not an
// entry for this list.
#include "cajeta/buildtool/FetchVerification.h"

namespace cajeta::buildtool {

    const std::vector<LegacyRelease>& legacyReleases() {
        static const std::vector<LegacyRelease> table = {
            {"dev.cajeta.cloud", "0.1.0", "sha256:20ea0d2a57cb17652791af85af61f830e288540e5c6c30971b94d0da9af05129"},
            {"dev.cajeta.coco", "0.2.0", "sha256:a823ad680f9c89195e9d035928768b1ff0312bb3199491809421a1bc8db2547f"},
            {"dev.cajeta.coco", "0.2.2", "sha256:662490e9df8d41af38bca27d1401ce1567a62a0fe7b2890476c89ecbac709e11"},
            {"dev.cajeta.coco", "0.5.0", "sha256:f0887d474149327182bb637e3db6115a19a75e250151757c09349911d2402f27"},
            {"dev.cajeta.coco", "0.5.1", "sha256:15c95f9ca530d75000b847f2395144e4d4838ebf282acb965aa347dd0f3fff79"},
            {"dev.cajeta.coco", "0.5.2", "sha256:d9749d78227f09a6d5ca47ec5982f4a928814017d33a83f5893f24c6538fb911"},
            {"dev.cajeta.coco", "0.6.0", "sha256:9316e3f2203cc659b5a84a3fd73801320697c5becdc9d9a1a473452e593e5b4e"},
            {"dev.cajeta.codec", "0.5.1", "sha256:36ef27f45e6b65962d6b5b04e2ade376b623592f69b3c05c8a91c41ee4eae65e"},
            {"dev.cajeta.codec", "0.6.0", "sha256:0caf203c9d884346ab1a08aaddc148c8c69779239136fad27848914c939cdf4d"},
            {"dev.cajeta.codec", "0.7.0", "sha256:81966df6cc27572c2a0059832540dd3f532c3fca8262665ce9f4bad8aede3e4b"},
            {"dev.cajeta.codec", "0.7.1", "sha256:14a5639b3a6a15bb631a5472ff0f406654559bdee3e660e8b0c83f8e0e4d80db"},
            {"dev.cajeta.codec", "0.8.0", "sha256:414d03e7499cf592f685fe6cb6b5d57d358735de9324e92ef901b814ec76fcb2"},
            {"dev.cajeta.codec", "0.8.1", "sha256:ffb0376d5eff82d282ff99cb1c5d9ccff7d9fd27392ed5b75572464e8a55c512"},
            {"dev.cajeta.codec", "0.8.2", "sha256:2894ff7219b85a3f383da9a09e7245c795ab197a5422247a49c74adf3eefa0d8"},
            {"dev.cajeta.coverage", "0.3.0", "sha256:086a6b24339bf77e650c5df90d4457a6fb7f6c3fb466bb5a1ce3729ba868bf26"},
            {"dev.cajeta.coverage", "0.5.0", "sha256:f0887d474149327182bb637e3db6115a19a75e250151757c09349911d2402f27"},
            {"dev.cajeta.coverage", "0.5.1", "sha256:fec0bd02e1cc309bd1cf81a76c9a059bf36acca77a87eeea6e207cec59c8dc13"},
            {"dev.cajeta.coverage", "0.5.2", "sha256:a6da8d49f2c7ad15dc767e48980937df90900b942c079c047ec0197304f86454"},
            {"dev.cajeta.coverage", "0.6.0", "sha256:64c348fd6982faaeaaa5b09ab6f1ac6102ba10bebbb17512cff5fece5e2d6834"},
            {"dev.cajeta.docs", "0.1.0", "sha256:0b0fba2c23a906f7dd5508201bd30585617a6c57a4652d05159e5af6afce9758"},
            {"dev.cajeta.gossip", "0.1.1", "sha256:2148c82a1ba7313f0f1fbc2a52a84e65ecb357c0eacc55a8a0623d0e75f43b5e"},
            {"dev.cajeta.graph", "0.1.0", "sha256:90877a906b100dac1fc55dcd4ba9170d64bd3266ed39301a9e90a321b5a926d6"},
            {"dev.cajeta.http", "0.1.1", "sha256:49a86d32ef2efec57ea433b7941e6d4b32e4048c2aef37d354a09ff39be05c8c"},
            {"dev.cajeta.http", "0.1.2", "sha256:988b5557c29041bfb17b1ecee518595c3d7e5a7f35ed55812772836992e11e37"},
            {"dev.cajeta.jinja", "0.1.0", "sha256:8255e9d3249f3283b2e146f696b8b34099094df4e32f6086b03586510c273ed1"},
            {"dev.cajeta.jinja", "0.1.1", "sha256:4b990123321f3a3f7536079dbdf8d427824e5754b27d6e2b3203bf9e6a909795"},
            {"dev.cajeta.logging", "0.6.0", "sha256:f27467c66f10718011b41bbfc3116bddf7cdc9126f54a5bf8bf6e4db37ae3ef2"},
            {"dev.cajeta.logging", "0.7.0", "sha256:f7ccbe0fc5ee0a34a56ce6d32967a943a78cbadc5209ba27554d3ce8611e8b55"},
            {"dev.cajeta.ml", "0.1.0", "sha256:3ae8bc8dbca9c9600ac47b2904731877d74cfbfadd744ef3e50a1a43229790ce"},
            {"dev.cajeta.ml", "0.2.0", "sha256:1618ac114cd4c9976638dbcaa23809bc56b9297a5fa1c08f0b42cd02644238da"},
            {"dev.cajeta.ml", "0.3.0", "sha256:a911354d028f7d76eba14737e49575e050beb8e405109339bc9fa97a7cc9e451"},
            {"dev.cajeta.ml", "0.4.0", "sha256:c2d22ea5285d3f7d73c0244296191a24a5c61d277d97fd61adedc99c35bc04f5"},
            {"dev.cajeta.ml", "0.6.0", "sha256:b2d1a808fd8595c9eb0d704190568851f692551612b518e733830b255541874b"},
            {"dev.cajeta.ml", "0.7.0", "sha256:3fcc88d0a205443965a2721de46c129651600f6df1b0c8d38c00fa29e95f8ab1"},
            {"dev.cajeta.ml", "0.8.0", "sha256:d2bc95ddad8127d640734949455892a52413a21958699cf232218f9a1b8597b9"},
            {"dev.cajeta.ml", "0.9.0", "sha256:2fde221a1929785b82522210537c6228f5901034bd0fe0ffb0fdb82dbf906252"},
            {"dev.cajeta.ml", "0.10.0", "sha256:75c1faed70153d77872a450973c26333ad17a62e1d57c4f71b489eb6eb10d6a6"},
            {"dev.cajeta.recsys", "0.1.0", "sha256:af60645e225988c19084efc6dce758c2d12aefb239ef28e5c68283218d9241bc"},
            {"dev.cajeta.timeseries", "0.1.0", "sha256:7395d1192b81a7fe9074a4605f4b36cffe625f529d449e4ca60962f7125c7962"},
            {"dev.cajeta.timeseries", "0.1.1", "sha256:ead1af8159ddb794da6015bd6854c0387d00065d721d5fdeacab4e7d07b8cddd"},
            {"dev.cajeta.unit", "0.2.0", "sha256:b512b1149897f231127fb2fadbf1f5e5e78a301402e5bcc1ea857d20d71eeaf2"},
            {"dev.cajeta.unit", "0.2.1", "sha256:d42ecda3aff66ed211a0bdb60cb73395f2b4da05b851389a67f5434e94d61413"},
            {"dev.cajeta.unit", "0.2.2", "sha256:10bc9083b1f4871a6c178ce37240d54a3499587d029d245534f138d878029a8e"},
            {"dev.cajeta.unit", "0.2.3", "sha256:a8e3d982f717dedbff386c4fbd8c2cf95488dcd114bad3427fc99713db8ee98c"},
            {"dev.cajeta.unit", "0.2.4", "sha256:a5807e4287926f80bfafe4242474e42961c4c04187d19d759ba274eada79b466"},
            {"dev.cajeta.xgboost", "0.1.0", "sha256:976956d83c2920c0bd7f2951652aade05b0ba1149b1b30789308f7b6d0039148"},
            {"dev.cajeta.xgboost", "0.2.0", "sha256:ced2d4cf7b69c8c3356c9306bfe04f3665461bed1c2e128fbafcea64e377fd02"},
            {"dev.cajeta.xgboost", "0.3.0", "sha256:7e811716b3a46ae985ced753f7295aaee359ced0b828f64a6555a96b54d36ff5"},
            {"dev.cajeta.xref", "0.1.0", "sha256:dfe3afd2244da247e61c47e6b6de2c7b86dc40007ad5c1dc6ae5ce9a46b33fdc"},
        };
        return table;
    }

} // namespace cajeta::buildtool
