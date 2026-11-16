# ComboPeel (PS Vita)

Plugin de kernel para PS Vita que desbloqueia a tela de bloqueio com um combo de botões,
fazendo a mesma animação de "descascar" (peel) do gesto com o dedo.

**Combo:** analógico esquerdo para baixo + X

## Como usar

1. Aperte o botão PS para acender a tela (aparece a tela de bloqueio com o relógio).
2. Incline o analógico esquerdo para baixo e aperte X.
3. O plugin arrasta um "dedo virtual" do canto superior direito até o centro, e a tela é desbloqueada.

### Proteções contra acionamento acidental

- O combo só funciona nos **20 segundos** depois que o Vita acorda do modo de espera
  (ou nos primeiros 2 minutos depois de ligar).
- Funciona **uma vez** por despertar.
- Se você tocar na tela, o combo é desativado até a próxima vez que o Vita dormir.
- É preciso apertar o combo (não adianta já estar segurando quando a tela acende).

## Instalação

Testado para firmware **3.65 com Ensō (HENkaku)**.

1. Baixe o `combo_peel.skprx` na aba **Actions** deste repositório
   (abra a execução mais recente com ✓ verde e baixe o artefato **ComboPeel**; ele vem dentro de um .zip).
2. Com o VitaShell, copie `combo_peel.skprx` para `ur0:tai/`.
3. Edite `ur0:tai/config.txt` e adicione esta linha logo abaixo de `*KERNEL`:

   ```
   *KERNEL
   ur0:tai/combo_peel.skprx
   ```

   > Se existir também `ux0:tai/config.txt`, é esse que o Vita usa. Edite o que estiver ativo.

4. Reinicie o Vita.

## Se algo der errado

- Se o Vita travar na inicialização, segure **L** enquanto liga para pular os plugins,
  e remova a linha do `config.txt`.

## Compilar

O GitHub Actions compila sozinho a cada envio. Para compilar localmente com o VitaSDK:

```
cmake -S . -B build
cmake --build build
```

## Como funciona

- Intercepta as funções de leitura do touch no kernel (`ksceTouchPeek/Read` e variantes `Region`)
  e, durante ~0,6 s, substitui os dados pelo gesto de um dedo virtual.
- Lê os botões com `ksceCtrlPeekBufferPositive`.
- Detecta o despertar com um callback de energia (`kscePowerRegisterCallback`).

Os parâmetros (botão, sensibilidade do analógico, janela de tempo, trajeto do gesto)
ficam no topo de `src/main.c`.
