<template>
  <b-card class="mb-4 mt-4">
    <b-card-title><DebugIcon class="mr-1"></DebugIcon>{{ $t("debug") }}</b-card-title>
    <p v-html="$t('debugDescription')"></p>

    <b-card-body>

      <b-table :items="tableItems" small borderless responsive="md" thead-class="hidden" class="mb-0"></b-table>

      <hr />
      <IndexDebugInfo v-for="idx of $store.state.sist2Info.indices" :key="idx.id" :index="idx" class="mt-2"></IndexDebugInfo>
    </b-card-body>
  </b-card>
</template>

<script>
import IndexDebugInfo from "@/components/IndexDebugInfo";
import DebugIcon from "@/components/icons/DebugIcon";

export default {
  name: "DebugInfo.vue",
  components: {DebugIcon, IndexDebugInfo},
  computed: {
    tableItems() {
      return [
        {key: "version", value: this.$store.state.sist2Info.version},
        {key: "platform", value: this.$store.state.sist2Info.platform},
        {key: "debugBinary", value: this.$store.state.sist2Info.debug},
        {key: "sist2CommitHash", value: this.$store.state.sist2Info.sist2Hash},
        {key: "esIndex", value: this.$store.state.sist2Info.esIndex},
        {key: "tagline", value: this.$store.state.sist2Info.tagline},
        {key: "dev", value: this.$store.state.sist2Info.dev},
        {key: "esVersion", value: this.$store.state.sist2Info.esVersion},
        {key: "esVersionSupported", value: this.$store.state.sist2Info.esVersionSupported},
        {key: "esVersionLegacy", value: this.$store.state.sist2Info.esVersionLegacy},
      ]
    }
  }
}
</script>