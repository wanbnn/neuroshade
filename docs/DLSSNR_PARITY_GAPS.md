# Equivalência do painel NR

Style, Preset, Intensity e Automatic Mask estão integrados à execução nativa e
persistem no perfil. Detalhes e limites em [DLSSNR_OVERLAY.md](DLSSNR_OVERLAY.md)
e [DLSSNR_ADDON_ANALYSIS.md](DLSSNR_ADDON_ANALYSIS.md).

- Style: três estilos comprovados no descritor; condicionamento style/128,
  validado contra dispatch capturado do companion com a constante correspondente.
- Preset: quatro seleções, mas apenas pesos do #1. Fallback reproduz o lookup da
  DLL e fica explícito no overlay; não há três modelos independentes.
- Intensity: força 0–2 em FP32 antes da quantização. Implementação nativa por
  escala da diferença neural. A composição completa NVIDIA não foi recuperada;
  equivalência RTX não foi testada. A mistura antiga strength segue independente.
- Automatic Mask e tom/estrutura/pele: controles derivados qualificados no grafo.

Ainda faltam UI Correction, máscaras externas, transferências de cor/HDR e
profundidade/movimento externos. Não anunciar paridade completa do renderizador
ou da qualidade/performance RTX com base apenas nesses testes.
