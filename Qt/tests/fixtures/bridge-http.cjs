// 发布包测试使用本地 HTTP 响应，保持真实上游协议模块参与执行。
const path = require('node:path');
const bridge = process.env.RHINE_TEST_BRIDGE;
const axios = require(
    require.resolve('axios', {
        paths: [bridge],
    }),
);
const upstreamCrypto = require(
    path.join(bridge, 'node_modules/@neteasecloudmusicapienhanced/api/util/crypto'),
);
axios.defaults.adapter = async (settings) => {
    const destination = (settings.baseURL || '') + settings.url;
    if (!/music\.163\.com|y\.qq\.com/.test(destination)) {
        throw new Error('Unexpected upstream');
    }
    const qq = destination.includes('qq.com');
    if (qq && process.env.QQ_AUTH_STATE_PATH !== process.env.RHINE_QQ_DEVICE_STATE_PATH) {
        throw new Error('QQ bridge discarded persistent device path');
    }
    let data = qq
        ? {
              code: 0,
              data: {
                  song: {
                      list: [
                          {
                              mid: 'fixtureMID',
                              name: 'QQ fixture',
                              singer: [],
                              album: {
                                  mid: 'albumMID',
                              },
                          },
                      ],
                      totalnum: 1,
                  },
                  topList: [
                      {
                          id: 4,
                          topTitle: 'Fixture chart',
                      },
                  ],
              },
          }
        : {
              code: 200,
              result: {
                  songs: [],
                  songCount: 0,
              },
              data: [
                  {
                      id: 1,
                      url: 'https://music.126.net/fixture',
                  },
              ],
          };
    if (qq && settings.params?.data) {
        const request = JSON.parse(settings.params.data);
        if (request.req_1?.method === 'GetAll') {
            data = {
                code: 0,
                req_1: {
                    code: 0,
                    data: {
                        group: [
                            {
                                toplist: [
                                    {
                                        topId: 4,
                                        title: 'Fixture chart',
                                        period: '2026-10-12',
                                    },
                                ],
                            },
                        ],
                    },
                },
            };
        } else if (request.req_1?.method === 'GetDetail') {
            if (request.req_1.param.period !== '2026-10-12') {
                throw new Error('Chart period changed');
            }
            data = {
                code: 0,
                req_1: {
                    code: 0,
                    data: {
                        data: {
                            totalNum: 1,
                        },
                        songInfoList: [
                            {
                                mid: 'fixtureMID',
                                name: 'QQ fixture',
                            },
                        ],
                    },
                },
            };
        }
    }
    if (destination.endsWith('/api/bsr/sk/get') || destination.includes('/xeapi/')) {
        throw new Error('Playback must not depend on unavailable xeapi key registration');
    }
    if (
        destination.includes('/song/enhance/player/url') &&
        !destination.split('?')[0].endsWith('/eapi/song/enhance/player/url')
    ) {
        throw new Error('Expected documented song_url fallback with upstream default protocol');
    }
    if (destination.split('?')[0].endsWith('/eapi/song/enhance/player/url')) {
        const payload = upstreamCrypto.eapiReqDecrypt(new URLSearchParams(settings.data).get('params')).data;
        data.data[0].requestedBr = payload.br;
        data.data[0].authenticated = payload.header.MUSIC_U === 'fixture-account';
        if (JSON.parse(payload.ids)[0] === '2') {
            data = {
                code: 403,
                message: 'fixture permission denied',
            };
        }
    }
    return {
        status: 200,
        data,
        headers: {},
        config: settings,
    };
};
