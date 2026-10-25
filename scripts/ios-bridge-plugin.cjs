const fs = require('node:fs');
const path = require('node:path');

// 避免 iOS 使用内置 Node.js 版本的 fetch
module.exports = {
    name: 'ios-utf8-validation',
    setup(build) {
        build.onLoad({ filter: /\/MusicBridge\/worker\.cjs$/ }, (args) => ({
            contents:
                `require(${JSON.stringify(path.join(path.dirname(args.path), 'ios-web-apis.cjs'))});\n` +
                fs.readFileSync(args.path, 'utf8'),
            loader: 'js',
        }));
        build.onLoad({ filter: /@neteasecloudmusicapienhanced\/api\/util\/crypto\.js$/ }, (args) => {
            const contents = fs.readFileSync(args.path, 'utf8');
            const decoder =
                /const decoder = new TextDecoder\('utf-8', \{ fatal: true \}\)\n\nconst isUtf8 = \(buf\) => \{\n  try \{\n    decoder.decode\(buf\)\n    return true\n  \} catch \{\n    return false\n  \}\n\}/g;
            const matches = [...contents.matchAll(decoder)];
            if (matches.length !== 1 || contents.match(/decoder\./g)?.length !== 1) {
                throw new Error('Upstream UTF-8 validator changed; review iOS compatibility before building');
            }
            return {
                contents: contents.replace(decoder, "const { isUtf8 } = require('node:buffer')"),
                loader: 'js',
            };
        });
    },
};
