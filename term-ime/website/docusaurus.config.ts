import {themes as prismThemes} from 'prism-react-renderer';
import type {Config} from '@docusaurus/types';
import type * as Preset from '@docusaurus/preset-classic';

const sourceUrl = 'https://github.com/fwz233-RE/C1auncher/tree/main/term-ime';

const config: Config = {
  title: 'term-ime',
  tagline: '在终端里直接输入中文——不管有没有桌面环境',
  favicon: 'img/favicon.svg',

  // Configuration for a future Pages deployment; this import does not deploy it.
  url: 'https://fwz233-re.github.io',
  baseUrl: '/C1auncher/',

  organizationName: 'fwz233-RE',
  projectName: 'C1auncher',

  onBrokenLinks: 'warn',
  onBrokenMarkdownLinks: 'warn',

  i18n: {
    defaultLocale: 'zh-CN',
    locales: ['zh-CN'],
  },

  presets: [
    [
      'classic',
      {
        docs: {
          sidebarPath: './sidebars.ts',
          editUrl: `${sourceUrl}/website/`,
        },
        blog: false,
        theme: {
          customCss: './src/css/custom.css',
        },
      } satisfies Preset.Options,
    ],
  ],

  themeConfig: {
    image: 'img/favicon.svg',
    colorMode: {
      defaultMode: 'dark',
      disableSwitch: true,
      respectPrefersColorScheme: false,
    },
    navbar: {
      title: 'term-ime',
      logo: {
        alt: 'term-ime',
        src: 'img/favicon.svg',
      },
      items: [
        {type: 'docSidebar', sidebarId: 'docs', position: 'left', label: '文档'},
        {href: sourceUrl, label: 'GitHub', position: 'right'},
      ],
    },
    footer: {
      style: 'dark',
      links: [
        {
          title: '文档',
          items: [
            {label: '快速开始', to: '/docs/quickstart'},
            {label: '快捷键', to: '/docs/shortcuts'},
            {label: '配置', to: '/docs/config'},
          ],
        },
        {
          title: '项目',
          items: [
            {label: 'GitHub', href: sourceUrl},
            {label: '主仓库发行版', href: 'https://github.com/fwz233-RE/C1auncher/releases'},
          ],
        },
      ],
      copyright: `Copyright © ${new Date().getFullYear()} term-ime. MIT License.`,
    },
    prism: {
      theme: prismThemes.github,
      darkTheme: prismThemes.dracula,
    },
  } satisfies Preset.ThemeConfig,
};

export default config;
