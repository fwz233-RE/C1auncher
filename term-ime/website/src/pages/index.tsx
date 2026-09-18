import type {ReactNode} from 'react';
import clsx from 'clsx';
import Link from '@docusaurus/Link';
import useDocusaurusContext from '@docusaurus/useDocusaurusContext';
import Layout from '@theme/Layout';
import Heading from '@theme/Heading';

import styles from './index.module.css';

function HomepageHeader() {
  const {siteConfig} = useDocusaurusContext();
  return (
    <header className={clsx('hero', styles.heroBanner)}>
      <div className="container">
        <div className={styles.heroInner}>
          <div className={styles.heroText}>
            <Heading as="h1" className={styles.heroTitle}>
              {siteConfig.title}
            </Heading>
            <p className={styles.heroTagline}>{siteConfig.tagline}</p>
            <p className={styles.heroDesc}>
              中文拼音输入，候选词智能排序，终端 UI 渲染，流畅的 TTY 打字体验。
            </p>
            <div className={styles.buttons}>
              <Link className={clsx('button button--primary button--lg', styles.buttonPrimary)} to="/docs/quickstart">
                快速开始
              </Link>
              <Link className={clsx('button button--secondary button--lg', styles.buttonSecondary)} to="/docs/shortcuts">
                快捷键
              </Link>
              <Link className={clsx('button button--secondary button--lg', styles.buttonSecondary)} to="https://github.com/fwz233-RE/C1auncher/tree/main/term-ime">
                GitHub
              </Link>
            </div>
            <div className={styles.scenario}>
              <span className={styles.scenarioIcon}>🖥</span>
              SSH 远程连接服务器，在纯终端环境中直接输入中文，无需桌面依赖。
            </div>
          </div>
          <div className={styles.heroDemo}>
            <div className={styles.demoChrome}>
              <span className={styles.dots}>
                <span className={styles.dotRed} />
                <span className={styles.dotYellow} />
                <span className={styles.dotGreen} />
              </span>
              <span className={styles.demoTitle}>term-ime — zsh</span>
            </div>
            <div className={styles.demoInner}>
              <div className={styles.demoContent}>
                <span className={styles.prompt}>$ </span>
                <span className={styles.output}>你好世界</span>
                <span className={styles.cursor} />
              </div>
              <div className={styles.statusBar}>
                <span className={styles.modeIndicator}>[拼]</span>
                <span className={styles.pinyin}> ni hao </span>
                <span className={styles.candidateSelected}> 1.你好 </span>
                <span className={styles.candidate}> 2.里 3.李 4.离 5.力 </span>
              </div>
            </div>
          </div>
        </div>
      </div>
    </header>
  );
}

function Features() {
  const features = [
    {title: '拼音输入', desc: '中文拼音输入法，逐字候选，数字键选词，流畅的 TTY 打字体验。'},
    {title: '零依赖', desc: '完全静态链接单文件，下载即用，无需安装任何系统库。'},
    {title: '自包含构建', desc: 'libuv、librime 全部源码内置，仅需标准构建工具链。'},
    {title: '完整中文支持', desc: 'UTF-8 编解码，CJK 宽字符正确对齐，候选词智能排序。'},
    {title: '终端 UI', desc: '候选栏、状态栏、设置面板，256色终端配色，清爽显示。'},
  ];

  return (
    <section className={styles.features}>
      <div className="container">
        <Heading as="h2" className={styles.sectionTitle}>特性</Heading>
        <div className={styles.featureGrid}>
          {features.map((f, i) => (
            <div key={i} className={styles.featureCard}>
              <div className={styles.featureCardInner}>
                <Heading as="h3" className={styles.featureCardTitle}>{f.title}</Heading>
                <p className={styles.featureCardDesc}>{f.desc}</p>
              </div>
            </div>
          ))}
        </div>
      </div>
    </section>
  );
}

function UseCases() {
  const cases = [
    'SSH 远程连接服务器，在纯终端环境中直接输入中文。运维人员写中文备注、配置注释、文档，不再受限于英文输入。',
    '麒麟 V10 / UOS / 方德 Server 版，无桌面最小化安装时，纯控制台与 SSH shell 下无任何中文输入能力。term-ime 不需要桌面、不需要 D-Bus、不需要 X。',
    'ARM64 / LoongArch / SW64 架构的国产服务器，通过源码编译即可在终端中输入中文。只需 gcc/cmake 和构建工具链。',
    'Docker 容器内使用：无桌面环境的容器镜像中运行，通过 SSH 或 docker exec 进入容器后可直接输入中文。',
    'CI/CD 流水线中的交互式调试：在 CI 执行环境中需要输入中文注释或日志时，无需安装图形组件。',
    'WSL (Windows Subsystem for Linux)：在 WSL 终端中直接输入中文，无需配置 Windows 输入法穿透。',
  ];

  return (
    <section className={styles.useCases}>
      <div className="container">
        <Heading as="h2" className={styles.sectionTitle}>使用场景</Heading>
        <div className={styles.useCaseList}>
          {cases.map((c, i) => (
            <div key={i} className={styles.useCaseItem}>
              <span className={styles.useCaseBullet}>▸</span>
              {c}
            </div>
          ))}
        </div>
      </div>
    </section>
  );
}

export default function Home(): ReactNode {
  const {siteConfig} = useDocusaurusContext();
  return (
    <Layout
      title={siteConfig.title}
      description="在终端里直接输入中文。SSH 远程服务器、Docker 容器、WSL、国产操作系统均可使用。无需桌面环境，无需 D-Bus，无需 X。">
      <HomepageHeader />
      <main>
        <Features />
        <UseCases />
      </main>
    </Layout>
  );
}