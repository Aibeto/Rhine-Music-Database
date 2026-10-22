import assert from 'node:assert/strict';
import { API } from '../../MusicBridge/client/netease.mjs';

let respond;
const acceptedCookies = [];
const api = new API({
    request(route, parameters, callback) {
        assert.equal(route, '/search');
        respond = callback;
    },
    acceptCookies(cookies, epoch) {
        acceptedCookies.push({ cookies, epoch });
    },
});
const response = {
    body: { code: 200, result: { songs: [] } },
    cookies: ['MUSIC_U=local-fixture'],
    _epoch: 3,
};

// 在响应到达前作废请求，旧 Cookie 不得进入当前账号状态。
let valid = true;
const staleRequest = api.request('/search', {}, [200], () => valid);
valid = false;
respond(JSON.stringify(response), '');
await assert.rejects(staleRequest, /Stale operation/);
assert.deepEqual(acceptedCookies, []);

// 只有有效且格式、状态码均通过的响应才能交付 Cookie 和请求结果。
const validRequest = api.request('/search');
respond(JSON.stringify(response), '');
assert.deepEqual(await validRequest, response.body);
assert.deepEqual(acceptedCookies, [{ cookies: JSON.stringify(response.cookies), epoch: 3 }]);

const deniedRequest = api.request('/search');
respond(JSON.stringify({ ...response, body: { code: 403, message: '本地权限拒绝' } }), '');
await assert.rejects(deniedRequest, /403/);
assert.equal(acceptedCookies.length, 1);

const malformedRequest = api.request('/search');
respond('{', '');
await assert.rejects(malformedRequest, /元数据格式无效/);
assert.equal(acceptedCookies.length, 1);
console.log('SDK 响应有效性和 Cookie 交付顺序检查通过');

// 喜欢歌曲 ID 顺序检查
const favoriteRequests = [];
const favoriteAPI = new API({
    acceptCookies() {},
    request(route, parameters, callback) {
        favoriteRequests.push(route);
        let body;
        if (route === '/user/playlist') {
            body = {
                code: 200,
                playlist: [
                    { id: 7, specialType: 0, creator: { userId: 42 } },
                    { id: 8, specialType: 5, creator: { userId: 99 } },
                    { id: 9, specialType: 5, creator: { userId: 42 } },
                ],
            };
        } else {
            assert.equal(route, '/playlist/detail');
            assert.equal(parameters.id, 9);
            body = { code: 200, playlist: { trackIds: [{ id: 30 }, { id: 10 }, { id: 20 }] } };
        }
        callback(JSON.stringify({ body }), '');
    },
});
assert.deepEqual(await favoriteAPI.likedIDs('42'), ['30', '10', '20']);
assert.deepEqual(favoriteRequests, ['/user/playlist', '/playlist/detail']);
console.log('喜欢歌曲使用本人喜欢歌单顺序检查通过');
